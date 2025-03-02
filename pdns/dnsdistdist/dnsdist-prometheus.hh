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
#pragma once

#ifndef DISABLE_PROMETHEUS
// Metric types for Prometheus
enum class PrometheusMetricType: uint8_t {
    counter = 1,
    gauge = 2
};

// Keeps additional information about metrics
struct MetricDefinition {
  MetricDefinition(PrometheusMetricType _prometheusType, const std::string& _description): description(_description), prometheusType(_prometheusType) {
  }

  MetricDefinition() = default;

  // Metric description
  std::string description;
  // Metric type for Prometheus
  PrometheusMetricType prometheusType{PrometheusMetricType::counter};
};

struct MetricDefinitionStorage {
  // Return metric definition by name
  bool getMetricDetails(const std::string& metricName, MetricDefinition& metric) const {
    const auto& metricDetailsIter = metrics.find(metricName);

    if (metricDetailsIter == metrics.end()) {
      return false;
    }

    metric = metricDetailsIter->second;
    return true;
  };

  static bool addMetricDefinition(const std::string& name, const std::string& type, const std::string& description) {
    static const std::map<std::string, PrometheusMetricType> namesToTypes = {
      {"counter", PrometheusMetricType::counter},
      {"gauge",   PrometheusMetricType::gauge},
    };
    auto realtype = namesToTypes.find(type);
    if (realtype == namesToTypes.end()) {
      return false;
    }
    metrics.emplace(name, MetricDefinition{realtype->second, description});
    return true;
  }

  // Return string representation of Prometheus metric type
  std::string getPrometheusStringMetricType(PrometheusMetricType metricType) const {
    switch (metricType) {
      case PrometheusMetricType::counter:
        return "counter";
        break;
      case PrometheusMetricType::gauge:
        return "gauge";
        break;
      default:
        return "";
        break;
    }
  };

  static std::map<std::string, MetricDefinition> metrics;
};
#endif /* DISABLE_PROMETHEUS */
