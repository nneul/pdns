/*
 * This file is part of PowerDNS or dnsdist.
 * Copyright -- PowerDNS.COM B.V. and its contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * In addition, for the avoidance of any doubt, permission is granted to
 * link this program with OpenSSL and to (re)distribute the binaries
 * produced as the result of such linking.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include <cinttypes>

#include "negcache.hh"
#include "misc.hh"
#include "cachecleaner.hh"
#include "utility.hh"
#include "rec-taskqueue.hh"

// For a description on how ServeStale works, see recursor_cache.cc, the general structure is the same.
uint16_t NegCache::s_maxServedStaleExtensions;

NegCache::NegCache(size_t mapsCount) :
  d_maps(mapsCount)
{
}

size_t NegCache::size() const
{
  size_t count = 0;
  for (const auto& map : d_maps) {
    count += map.d_entriesCount;
  }
  return count;
}

/*!
 * Set ne to the NegCacheEntry for the last label in qname and return true if there
 * was one.
 *
 * \param qname    The name to look up (only the last label is used)
 * \param now      A timeval with the current time, to check if an entry is expired
 * \param ne       A NegCacheEntry that is filled when there is a cache entry
 * \return         true if ne was filled out, false otherwise
 */
bool NegCache::getRootNXTrust(const DNSName& qname, const struct timeval& now, NegCacheEntry& ne, bool serveStale, bool refresh)
{
  // Never deny the root.
  if (qname.isRoot())
    return false;

  // An 'ENT' QType entry, used as "whole name" in the neg-cache context.
  static const QType qtnull(0);
  DNSName lastLabel = qname.getLastLabel();

  auto& map = getMap(lastLabel);
  auto content = map.lock();

  negcache_t::const_iterator ni = content->d_map.find(std::tie(lastLabel, qtnull));

  while (ni != content->d_map.end() && ni->d_name == lastLabel && ni->d_auth.isRoot() && ni->d_qtype == qtnull) {
    if (!refresh && (serveStale || ni->d_servedStale > 0) && ni->d_ttd <= now.tv_sec && ni->d_servedStale < s_maxServedStaleExtensions) {
      updateStaleEntry(now.tv_sec, ni);
    }
    // We have something
    if (now.tv_sec < ni->d_ttd) {
      ne = *ni;
      moveCacheItemToBack<SequenceTag>(content->d_map, ni);
      return true;
    }
    if (ni->d_servedStale == 0 && !serveStale) {
      moveCacheItemToFront<SequenceTag>(content->d_map, ni);
    }
    ++ni;
  }
  return false;
}

void NegCache::updateStaleEntry(time_t now, negcache_t::iterator& entry)
{
  // We need to take care a infrequently access stale item cannot be extended past
  // s_maxServedStaleExtension * s_serveStaleExtensionPeriod
  // We we look how old the entry is, and increase d_servedStale accordingly, taking care not to overflow
  const time_t howlong = std::max(static_cast<time_t>(1), now - entry->d_ttd);
  const uint32_t extension = std::max(1U, std::min(entry->d_orig_ttl, s_serveStaleExtensionPeriod));
  entry->d_servedStale = std::min(entry->d_servedStale + 1 + howlong / extension, static_cast<time_t>(s_maxServedStaleExtensions));
  entry->d_ttd = now + std::min(entry->d_orig_ttl, s_serveStaleExtensionPeriod);

  pushAlmostExpiredTask(entry->d_name, entry->d_qtype == 0 ? QType::A : entry->d_qtype, entry->d_ttd, Netmask());
}

/*!
 * Set ne to the NegCacheEntry for the qname|qtype tuple and return true
 *
 * \param qname    The name to look up
 * \param qtype    The qtype to look up
 * \param now      A timeval with the current time, to check if an entry is expired
 * \param ne       A NegCacheEntry that is filled when there is a cache entry
 * \return         true if ne was filled out, false otherwise
 */
bool NegCache::get(const DNSName& qname, const QType& qtype, const struct timeval& now, NegCacheEntry& ne, bool typeMustMatch, bool serveStale, bool refresh)
{
  auto& map = getMap(qname);
  auto content = map.lock();

  const auto& idx = content->d_map.get<NegCacheEntry>();
  auto range = idx.equal_range(qname);
  auto ni = range.first;

  while (ni != range.second) {
    // We have an entry
    if ((!typeMustMatch && ni->d_qtype.getCode() == 0) || ni->d_qtype == qtype) {
      // We match the QType or the whole name is denied
      auto firstIndexIterator = content->d_map.project<CompositeKey>(ni);

      if (!refresh && (serveStale || ni->d_servedStale > 0) && ni->d_ttd <= now.tv_sec && ni->d_servedStale < s_maxServedStaleExtensions) {
        updateStaleEntry(now.tv_sec, firstIndexIterator);
      }
      if (now.tv_sec < ni->d_ttd && !(refresh && ni->d_servedStale > 0)) {
        // Not expired
        ne = *ni;
        moveCacheItemToBack<SequenceTag>(content->d_map, firstIndexIterator);
        return true;
      }
      // expired
      moveCacheItemToFront<SequenceTag>(content->d_map, firstIndexIterator);
    }
    ++ni;
  }
  return false;
}

/*!
 * Places ne into the negative cache, possibly overriding an existing entry.
 *
 * \param ne The NegCacheEntry to add to the cache
 */
void NegCache::add(const NegCacheEntry& ne)
{
  bool inserted = false;
  auto& map = getMap(ne.d_name);
  auto content = map.lock();
  inserted = lruReplacingInsert<SequenceTag>(content->d_map, ne);
  if (inserted) {
    ++map.d_entriesCount;
  }
}

/*!
 * Update the validation state of an existing entry with the provided state.
 *
 * \param qname The name of the entry to replace
 * \param qtype The type of the entry to replace
 * \param newState The new validation state
 */
void NegCache::updateValidationStatus(const DNSName& qname, const QType& qtype, const vState newState, boost::optional<time_t> capTTD)
{
  auto& mc = getMap(qname);
  auto map = mc.lock();
  auto range = map->d_map.equal_range(std::tie(qname, qtype));

  if (range.first != range.second) {
    range.first->d_validationState = newState;
    if (capTTD) {
      range.first->d_ttd = std::min(range.first->d_ttd, *capTTD);
    }
  }
}

/*!
 * Returns the amount of entries in the cache
 *
 * \param qname The name of the entries to be counted
 */
size_t NegCache::count(const DNSName& qname)
{
  auto& map = getMap(qname);
  auto content = map.lock();
  return content->d_map.count(std::tie(qname));
}

/*!
 * Returns the amount of entries in the cache for qname+qtype
 *
 * \param qname The name of the entries to be counted
 * \param qtype The type of the entries to be counted
 */
size_t NegCache::count(const DNSName& qname, const QType qtype)
{
  auto& map = getMap(qname);
  auto content = map.lock();
  return content->d_map.count(std::tie(qname, qtype));
}

/*!
 * Remove all entries for name from the cache. If subtree is true, wipe all names
 * underneath it.
 *
 * \param name    The DNSName of the entries to wipe
 * \param subtree Should all entries under name be removed?
 */
size_t NegCache::wipe(const DNSName& name, bool subtree)
{
  size_t ret = 0;
  if (subtree) {
    for (auto& map : d_maps) {
      auto m = map.lock();
      for (auto i = m->d_map.lower_bound(std::tie(name)); i != m->d_map.end();) {
        if (!i->d_name.isPartOf(name))
          break;
        i = m->d_map.erase(i);
        ret++;
        --map.d_entriesCount;
      }
    }
    return ret;
  }

  auto& map = getMap(name);
  auto content = map.lock();
  auto range = content->d_map.equal_range(std::tie(name));
  auto i = range.first;
  while (i != range.second) {
    i = content->d_map.erase(i);
    ret++;
    --map.d_entriesCount;
  }
  return ret;
}

/*!
 * Clear the negative cache
 */
void NegCache::clear()
{
  for (auto& map : d_maps) {
    auto m = map.lock();
    m->d_map.clear();
    map.d_entriesCount = 0;
  }
}

/*!
 * Perform some cleanup in the cache, removing stale entries
 *
 * \param maxEntries The maximum number of entries that may exist in the cache.
 */
void NegCache::prune(size_t maxEntries)
{
  size_t cacheSize = size();
  pruneMutexCollectionsVector<SequenceTag>(*this, d_maps, maxEntries, cacheSize);
}

/*!
 * Writes the whole negative cache to fp
 *
 * \param fp A pointer to an open FILE object
 */
size_t NegCache::dumpToFile(FILE* fp, const struct timeval& now)
{
  size_t ret = 0;

  for (auto& mc : d_maps) {
    auto m = mc.lock();
    auto& sidx = m->d_map.get<SequenceTag>();
    for (const NegCacheEntry& ne : sidx) {
      ret++;
      int64_t ttl = ne.d_ttd - now.tv_sec;
      fprintf(fp, "%s %" PRId64 " IN %s VIA %s ; (%s) origttl=%" PRIu32 " ss=%hu\n", ne.d_name.toString().c_str(), ttl, ne.d_qtype.toString().c_str(), ne.d_auth.toString().c_str(), vStateToString(ne.d_validationState).c_str(), ne.d_orig_ttl, ne.d_servedStale);
      for (const auto& rec : ne.authoritySOA.records) {
        fprintf(fp, "%s %" PRId64 " IN %s %s ; (%s)\n", rec.d_name.toString().c_str(), ttl, DNSRecordContent::NumberToType(rec.d_type).c_str(), rec.d_content->getZoneRepresentation().c_str(), vStateToString(ne.d_validationState).c_str());
      }
      for (const auto& sig : ne.authoritySOA.signatures) {
        fprintf(fp, "%s %" PRId64 " IN RRSIG %s ;\n", sig.d_name.toString().c_str(), ttl, sig.d_content->getZoneRepresentation().c_str());
      }
      for (const auto& rec : ne.DNSSECRecords.records) {
        fprintf(fp, "%s %" PRId64 " IN %s %s ; (%s)\n", rec.d_name.toString().c_str(), ttl, DNSRecordContent::NumberToType(rec.d_type).c_str(), rec.d_content->getZoneRepresentation().c_str(), vStateToString(ne.d_validationState).c_str());
      }
      for (const auto& sig : ne.DNSSECRecords.signatures) {
        fprintf(fp, "%s %" PRId64 " IN RRSIG %s ;\n", sig.d_name.toString().c_str(), ttl, sig.d_content->getZoneRepresentation().c_str());
      }
    }
  }
  return ret;
}
