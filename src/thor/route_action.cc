#include <prime_server/prime_server.hpp>

using namespace prime_server;

#include <valhalla/midgard/logging.h>
#include <valhalla/midgard/constants.h>
#include <valhalla/baldr/json.h>
#include <valhalla/sif/autocost.h>
#include <valhalla/sif/bicyclecost.h>
#include <valhalla/sif/pedestriancost.h>

#include "thor/service.h"

using namespace valhalla;
using namespace valhalla::midgard;
using namespace valhalla::baldr;
using namespace valhalla::sif;
using namespace valhalla::thor;


namespace {

  const headers_t::value_type CORS{"Access-Control-Allow-Origin", "*"};
  const headers_t::value_type JSON_MIME{"Content-type", "application/json;charset=utf-8"};
  const headers_t::value_type JS_MIME{"Content-type", "application/javascript;charset=utf-8"};

}

namespace valhalla {
  namespace thor {

  worker_t::result_t thor_worker_t::route(const boost::property_tree::ptree& request, const std::string &request_str, const boost::optional<int> &date_time_type, const bool header_dnt){
    parse_locations(request);
    auto costing = parse_costing(request);

    worker_t::result_t result{true};
    //get time for start of request
    auto s = std::chrono::system_clock::now();
    // Forward the original request
    result.messages.emplace_back(request_str);

    auto trippaths = (date_time_type && *date_time_type == 2) ?
        path_arrive_by(correlated, costing, request_str) :
        path_depart_at(correlated, costing, date_time_type, request_str);
    for (const auto &trippath: trippaths) {
      result.messages.emplace_back(trippath.SerializeAsString());
    }
    //get processing time for thor
    auto e = std::chrono::system_clock::now();
    std::chrono::duration<float, std::milli> elapsed_time = e - s;
    //log request if greater than X (ms)
    if (!header_dnt && (elapsed_time.count() / correlated.size()) > long_request) {
     LOG_WARN("thor::route trip_path elapsed time (ms)::"+ std::to_string(elapsed_time.count()));
     LOG_WARN("thor::route trip_path exceeded threshold::"+ request_str);
     midgard::logging::Log("valhalla_thor_long_request_route", " [ANALYTICS] ");
    }
    return result;
  }

  /**
   * Update the origin edges for a through location.
   */
  void thor_worker_t::update_origin(baldr::PathLocation& origin, bool prior_is_node,
                    const baldr::GraphId& through_edge) {
    if (prior_is_node) {
      // TODO - remove the opposing through edge from list of edges unless
      // all outbound edges are entering noth_thru regions.
      // For now allow all edges
    } else {
      // Check if the edge is entering a not_thru region - if so do not
      // exclude the opposing edge
      const DirectedEdge* de = reader.GetGraphTile(through_edge)->directededge(through_edge);
      if (de->not_thru()) {
        return;
      }

      // Check if the through edge is dist = 1 (through point is at a node)
      bool ends_at_node = false;;
      for (const auto& e : origin.edges) {
        if (e.id == through_edge) {
          if (e.end_node()) {
            ends_at_node = true;
            break;
          }
        }
      }

      // Special case if location is at the end of a through edge
      if (ends_at_node) {
        // Erase the through edge and its opposing edge (if in the list)
        // from the origin edges
        auto opp_edge = reader.GetOpposingEdgeId(through_edge);
        std::remove_if(origin.edges.begin(), origin.edges.end(),
           [&through_edge, &opp_edge](const PathLocation::PathEdge& edge) {
              return edge.id == through_edge || edge.id == opp_edge; });
      } else {
        // Set the origin edge to the through_edge.
        for (auto e : origin.edges) {
          if (e.id == through_edge) {
            origin.edges.clear();
            origin.edges.push_back(e);
            break;
          }
        }
      }
    }
  }

  thor::PathAlgorithm* thor_worker_t::get_path_algorithm(const std::string& routetype,
        const baldr::PathLocation& origin, const baldr::PathLocation& destination) {
    if (routetype == "multimodal" || routetype == "transit") {
      return &multi_modal_astar;
    } else {
      // Use A* if any origin and destination edges are the same - otherwise
      // use bidirectional A*. Bidirectional A* does not handle trivial cases
      // with oneways.
      for (auto& edge1 : origin.edges) {
        for (auto& edge2 : destination.edges) {
          if (edge1.id == edge2.id) {
            return &astar;
          }
        }
      }
      return &bidir_astar;
    }
  }

  void thor_worker_t::get_path(PathAlgorithm* path_algorithm,
               baldr::PathLocation& origin, baldr::PathLocation& destination,
               std::vector<thor::PathInfo>& path_edges) {
    midgard::logging::Log("#_passes::1", " [ANALYTICS] ");
    // Find the path.
    path_edges = path_algorithm->GetBestPath(origin, destination, reader,
                                             mode_costing, mode);
    // If path is not found try again with relaxed limits (if allowed)
    if (path_edges.size() == 0) {
      valhalla::sif::cost_ptr_t cost = mode_costing[static_cast<uint32_t>(mode)];
      if (cost->AllowMultiPass()) {
        // 2nd pass. Less aggressive hierarchy transitioning
        path_algorithm->Clear();
        bool using_astar = (path_algorithm == &astar);
        float relax_factor = using_astar ? 16.0f : 8.0f;
        float expansion_within_factor = using_astar ? 4.0f : 2.0f;
        cost->RelaxHierarchyLimits(relax_factor, expansion_within_factor);
        midgard::logging::Log("#_passes::2", " [ANALYTICS] ");
        path_edges = path_algorithm->GetBestPath(origin, destination,
                                  reader, mode_costing, mode);

        // 3rd pass (only for A*)
        if (path_edges.size() == 0 && using_astar) {
          path_algorithm->Clear();
          cost->DisableHighwayTransitions();
          midgard::logging::Log("#_passes::3", " [ANALYTICS] ");
          path_edges = path_algorithm->GetBestPath(origin, destination,
                                   reader, mode_costing, mode);
        }
      }
    }
  }

  std::list<valhalla::odin::TripPath> thor_worker_t::path_arrive_by(std::vector<PathLocation>& correlated, const std::string &costing, const std::string &request_str) {
    //get time for start of request
    auto s = std::chrono::system_clock::now();
    // For each pair of origin/destination
    bool prior_is_node = false;
    baldr::GraphId through_edge;
    std::vector<baldr::PathLocation> through_loc;
    std::vector<thor::PathInfo> path_edges;
    std::string origin_date_time;

    std::list<valhalla::odin::TripPath> trippaths;
    baldr::PathLocation& last_break_dest = *correlated.rbegin();

    for(auto path_location = ++correlated.crbegin(); path_location != correlated.crend(); ++path_location) {
      auto origin = *path_location;
      auto destination = *std::prev(path_location);

      // Through edge is valid if last orgin was "through"
      if (through_edge.Is_Valid()) {
        update_origin(origin, prior_is_node, through_edge);
      } else {
        last_break_dest = destination;
      }

      // Get the algorithm type for this location pair
      thor::PathAlgorithm* path_algorithm = get_path_algorithm(costing,
                           origin, destination);

      // Get best path
      if (path_edges.size() == 0) {
        get_path(path_algorithm, origin, destination, path_edges);
        if (path_edges.size() == 0) {
          throw valhalla_exception_t{400, 442};
        }
      } else {
        // Get the path in a temporary vector
        std::vector<thor::PathInfo> temp_path;
        get_path(path_algorithm, origin, destination, temp_path);
        if (temp_path.size() == 0) {
          throw valhalla_exception_t{400, 442};
        }

        // Append the temp_path edges to path_edges, adding the elapsed
        // time from the end of the current path. If continuing along the
        // same edge, remove the prior so we do not get a duplicate edge.
        uint32_t t = path_edges.back().elapsed_time;
        if (temp_path.front().edgeid == path_edges.back().edgeid) {
          path_edges.pop_back();
        }
        for (auto edge : temp_path) {
          edge.elapsed_time += t;
          path_edges.emplace_back(edge);
        }
      }

      // Build trip path for this leg and add to the result if this
      // location is a BREAK or if this is the last location
      if (origin.stoptype_ == Location::StopType::BREAK ||
          path_location == --correlated.crend()) {

          if (!origin_date_time.empty())
            last_break_dest.date_time_ = origin_date_time;

          // Form output information based on path edges
          auto trip_path = thor::TripPathBuilder::Build(reader, mode_costing,
                       path_edges, origin, last_break_dest, through_loc);

          if (origin.date_time_)
            origin_date_time = *origin.date_time_;

          // The protobuf path
          trippaths.emplace_front(std::move(trip_path));

          // Clear path edges and set through edge to invalid
          path_edges.clear();
          through_edge = baldr::GraphId();
      } else {
          // This is a through location. Save last edge as the through_edge
          prior_is_node = false;
          for(const auto& e : origin.edges) {
            if(e.id == path_edges.back().edgeid) {
              prior_is_node = e.begin_node() || e.end_node();
              break;
            }
          }
          through_edge = path_edges.back().edgeid;

          // Add to list of through locations for this leg
          through_loc.emplace_back(origin);
      }

      // If we have another one coming we need to clear
      if (--correlated.crend() != path_location)
        path_algorithm->Clear();
    }

    return trippaths;
  }

  std::list<valhalla::odin::TripPath> thor_worker_t::path_depart_at(std::vector<PathLocation>& correlated, const std::string &costing, const boost::optional<int> &date_time_type, const std::string &request_str) {
    //get time for start of request
    auto s = std::chrono::system_clock::now();
    bool prior_is_node = false;
    std::vector<baldr::PathLocation> through_loc;
    baldr::GraphId through_edge;
    std::vector<thor::PathInfo> path_edges;
    std::string origin_date_time, dest_date_time;

    std::list<valhalla::odin::TripPath> trippaths;
    baldr::PathLocation& last_break_origin = correlated[0];
    for(auto path_location = ++correlated.cbegin(); path_location != correlated.cend(); ++path_location) {
      auto origin = *std::prev(path_location);
      auto destination = *path_location;

      if (date_time_type && (*date_time_type == 0 || *date_time_type == 1) &&
          !dest_date_time.empty() && origin.stoptype_ == Location::StopType::BREAK)
        origin.date_time_ = dest_date_time;

      // Through edge is valid if last destination was "through"
      if (through_edge.Is_Valid()) {
        update_origin(origin, prior_is_node, through_edge);
      } else {
        last_break_origin = origin;
      }

      // Get the algorithm type for this location pair
      thor::PathAlgorithm* path_algorithm = get_path_algorithm(costing,
                           origin, destination);

      // Get best path
      if (path_edges.size() == 0) {
        get_path(path_algorithm, origin, destination, path_edges);
        if (path_edges.size() == 0) {
          throw valhalla_exception_t{400, 442};
        }

        if (date_time_type && *date_time_type == 0 && origin_date_time.empty() &&
            origin.stoptype_ == Location::StopType::BREAK)
          last_break_origin.date_time_ = origin.date_time_;
      } else {
        // Get the path in a temporary vector
        std::vector<thor::PathInfo> temp_path;
        get_path(path_algorithm, origin, destination, temp_path);
        if (temp_path.size() == 0) {
          throw valhalla_exception_t{400, 442};
        }

        if (date_time_type && *date_time_type == 0 && origin_date_time.empty() &&
            origin.stoptype_ == Location::StopType::BREAK)
          last_break_origin.date_time_ = origin.date_time_;

        // Append the temp_path edges to path_edges, adding the elapsed
        // time from the end of the current path. If continuing along the
        // same edge, remove the prior so we do not get a duplicate edge.
        uint32_t t = path_edges.back().elapsed_time;
        if (temp_path.front().edgeid == path_edges.back().edgeid) {
          path_edges.pop_back();
        }
        for (auto edge : temp_path) {
          edge.elapsed_time += t;
          path_edges.emplace_back(edge);
        }
      }

      // Build trip path for this leg and add to the result if this
      // location is a BREAK or if this is the last location
      if (destination.stoptype_ == Location::StopType::BREAK ||
          path_location == --correlated.cend()) {
          // Form output information based on path edges
          auto trip_path = thor::TripPathBuilder::Build(reader, mode_costing,
                   path_edges, last_break_origin, destination, through_loc);

          if (date_time_type) {
            origin_date_time = *last_break_origin.date_time_;
            dest_date_time = *destination.date_time_;
          }

          // The protobuf path
          trippaths.emplace_back(std::move(trip_path));

          // Clear path edges and set through edge to invalid
          path_edges.clear();
          through_edge = baldr::GraphId();
      } else {
          // This is a through location. Save last edge as the through_edge
          prior_is_node = false;
          for(const auto& e : origin.edges) {
            if(e.id == path_edges.back().edgeid) {
              prior_is_node = e.begin_node() || e.end_node();
              break;
            }
          }
          through_edge = path_edges.back().edgeid;

          // Add to list of through locations for this leg
          through_loc.emplace_back(destination);
      }

      // If we have another one coming we need to clear
      if (--correlated.cend() != path_location)
        path_algorithm->Clear();
    }

    return trippaths;
  }

  }
}
