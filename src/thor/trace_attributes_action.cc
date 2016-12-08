#include <prime_server/prime_server.hpp>

using namespace prime_server;

#include <valhalla/baldr/json.h>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <valhalla/midgard/logging.h>
#include <valhalla/midgard/constants.h>
#include <valhalla/baldr/errorcode_util.h>
#include <valhalla/proto/trippath.pb.h>

#include "thor/service.h"
#include "thor/trip_path_controller.h"

using namespace valhalla;
using namespace valhalla::midgard;
using namespace valhalla::baldr;
using namespace valhalla::sif;
using namespace valhalla::odin;
using namespace valhalla::thor;


namespace {
  const headers_t::value_type CORS { "Access-Control-Allow-Origin", "*" };
  const headers_t::value_type JSON_MIME { "Content-type", "application/json;charset=utf-8" };
  const headers_t::value_type JS_MIME { "Content-type", "application/javascript;charset=utf-8" };


  json::MapPtr serialize(valhalla::odin::TripPath trip_path, const boost::optional<std::string>& id, double scale) {
    //lets get some edge attributes
    json::ArrayPtr edges = json::array({});
    for (int i = 1; i < trip_path.node().size(); i++) {

    //  if (trip_path.has_node()) {
        auto node = trip_path.node(i);
        auto end_node = json::array({});
        auto intersecting_edges = json::array({});
        if (node.intersecting_edge().size() > 0) {
          for (const auto& intersecting_edge : node.intersecting_edge())
            intersecting_edges->push_back(static_cast<uint64_t>(intersecting_edge.begin_heading()));
        }

        end_node->emplace_back(json::map({
          {"intersecting_edges", intersecting_edges}
        }));
    //  }

      if (trip_path.node(i-1).has_edge()) {
        const auto& edge = trip_path.node(i - 1).edge();
        auto names = json::array({});
        if (edge.name().size() > 0) {
          for (const auto& name : edge.name())
            names->push_back(name);
        }

        auto edgemap = json::map({});
        if (edge.has_max_downward_grade())
          edgemap->emplace("max_downward_grade", static_cast<int64_t>(edge.max_downward_grade()));
        if (edge.has_max_upward_grade())
          edgemap->emplace("max_upward_grade", static_cast<int64_t>(edge.max_upward_grade()));
        if (edge.has_weighted_grade())
          edgemap->emplace("weighted_grade", json::fp_t{edge.weighted_grade(), 3});
        if (edge.has_length())
          edgemap->emplace("length", json::fp_t{edge.length() * scale, 3});
        if (edge.has_speed())
          edgemap->emplace("speed", json::fp_t{edge.speed() * scale, 3});
        if (edge.has_way_id())
          edgemap->emplace("way_id", static_cast<uint64_t>(edge.way_id()));
        if (edge.has_id())
          edgemap->emplace("id", static_cast<uint64_t>(edge.id()));
        if (edge.has_end_shape_index())
          edgemap->emplace("end_shape_index", static_cast<int64_t>(edge.end_shape_index()));
        if (edge.has_begin_shape_index())
          edgemap->emplace("begin_shape_index", static_cast<int64_t>(edge.begin_shape_index()));
        //if (trip_path.has_node())
        edgemap->emplace("end_node", end_node);
        if (names)
          edgemap->emplace("names", names);

        edges->emplace_back(edgemap);

        auto json = json::map({
          {"edges", edges}
        });
        if (id)
          json->emplace("id", *id);
        if (edge.has_begin_shape_index() || edge.has_end_shape_index())
          json->emplace("shape", trip_path.shape());

    return json;
      }
    }
  }
}

namespace valhalla {
namespace thor {

void thor_worker_t::filter_attributes(const boost::property_tree::ptree& request, TripPathController& controller) {
  std::string filter_action = request.get("filters.action", "");

  if (filter_action.size() && filter_action == "only") {
    controller.disable_all();
    for (const auto& kv : request.get_child("filters.attributes"))
      attributes_include_.emplace(kv.second.get_value<std::string>());

    for (const auto& include : attributes_include_)
      controller.attributes.at(include) = true;

  } else if (filter_action.size() && filter_action == "none") {
    controller.enable_all();
    for (const auto& kv : request.get_child("filters.attributes"))
      attributes_exclude_.emplace(kv.second.get_value<std::string>());

    for (const auto& exclude : attributes_exclude_)
      controller.attributes.at(exclude) = false;

  } else {
    controller.disable_all();
    //TODO:  This default will change
    controller.attributes.at(kEdgeNames) = true;
    controller.attributes.at(kEdgeId) = true;
    controller.attributes.at(kEdgeWayId) = true;
    controller.attributes.at(kEdgeSpeed) = true;
    controller.attributes.at(kEdgeLength) = true;
    controller.attributes.at(kEdgeWeightedGrade) = true;
    controller.attributes.at(kEdgeMaxUpwardGrade) = true;
    controller.attributes.at(kEdgeMaxDownwardGrade) = true;
  }
}

/*
 * The trace_attributes action takes a GPS trace or latitude, longitude positions
 * from a portion of an existing route and returns detailed attribution along the
 * portion of the route. This includes details for each section of road along the
 * path as well as any intersections along the path.
 */
worker_t::result_t thor_worker_t::trace_attributes(
    const boost::property_tree::ptree &request,
    const std::string &request_str, http_request_t::info_t& request_info) {
  //get time for start of request
  auto s = std::chrono::system_clock::now();

  // Parse request
  parse_locations(request);
  parse_shape(request);
  parse_costing(request);
  parse_trace_config(request);
  /*
   * A flag indicating whether the input shape is a GPS trace or exact points from a
   * prior route run against the Valhalla road network.  Knowing that the input is from
   * Valhalla will allow an efficient “edge-walking” algorithm rather than a more extensive
   * map-matching method. If true, this enforces to only use exact route match algorithm.
   */
  odin::TripPath trip_path;
  TripPathController controller;
  auto shape_match = request.get<std::string>("shape_match", "edge_walk");
  filter_attributes(request, controller);

  // If the exact points from a prior route that was run agains the Valhalla road network,
  //then we can traverse the exact shape to form a path by using edge-walking algorithm
  if (shape_match == "edge_walk") {
    try {
      trip_path = route_match(controller);
    } catch (...) {
      LOG_INFO("Could not find exact route match.  Use shape_match:'walk or snap' to fallback to map-matching algorithm");
      valhalla_exception_t{400, 443};
    }
  // If non-exact shape points are used, then we need to correct this shape by sending them
  // through the map-matching algorithm to snap the points to the correct shape
  } else if (shape_match == "map_snap") {
    try {
      trip_path = map_match(controller);
    } catch (...) {
      LOG_INFO("Map-matching algorithm failed to snap the shape points to the correct shape.");
      valhalla_exception_t{400, 444};
    }
  //If we think that we have the exact shape but there ends up being no Valhalla route match, then
  // then we want to fallback to try and use meili map matching to match to local route network.
  //No shortcuts are used and detailed information at every intersection becomes available.
  } else if (shape_match == "walk_or_snap") {
    trip_path = route_match(controller);
    if (trip_path.node().size() == 0) {
      LOG_INFO("Could not find exact route match; Sending trace to map_match...");
      try {
        trip_path = map_match(controller);
      } catch (...) {
        LOG_INFO("Map-matching algorithm failed to snap the shape points to the correct shape.");
        valhalla_exception_t{400, 444};
      }
    }
  }
  auto id = request.get_optional<std::string>("id");
  //length and speed default to km
  double scale = 1;
  auto units = request.get<std::string>("units", "km");
  if ((units == "mi") || (units == "miles"))
    scale = kMilePerKm;

  //serialize output to Thor
  json::MapPtr json;
  if (trip_path.node().size() > 0)
    json = serialize(trip_path, id, scale);
  else throw valhalla_exception_t{400, 442};

  //jsonp callback if need be
  std::ostringstream stream;
  auto jsonp = request.get_optional<std::string>("jsonp");
  if (jsonp)
    stream << *jsonp << '(';
  stream << *json;
  if (jsonp)
    stream << ')';

  // Get processing time for thor
  auto e = std::chrono::system_clock::now();
  std::chrono::duration<float, std::milli> elapsed_time = e - s;
  // TODO determine what to log
  //log request if greater than X (ms)
  if (!request_info.do_not_track && (elapsed_time.count() / correlated.size()) > long_request) {
    LOG_WARN("thor::trace_attributes elapsed time (ms)::"+ std::to_string(elapsed_time.count()));
    LOG_WARN("thor::trace_attributes exceeded threshold::"+ request_str);
    midgard::logging::Log("valhalla_thor_long_request_trace_attributes", " [ANALYTICS] ");
  }
  http_response_t response(200, "OK", stream.str(), headers_t{CORS, jsonp ? JS_MIME : JSON_MIME});
  response.from_info(request_info);
  worker_t::result_t result{false};
  result.messages.emplace_back(response.to_string());
  return result;
}
}
}
