/**
 * @file ukv_graph_pfor.cpp
 * @author Ashot Vardanian
 *
 * @brief Graph implementation using fast integer compression.
 * Sits on top of any @see "ukv.h"-compatiable system.
 */

#include <numeric>  // `std::accumulate`
#include <optional> // `std::optional`
#include <limits>   // `std::numeric_limits`

#include "ukv_graph.hpp"
#include "helpers.hpp"

/*********************************************************/
/*****************	 C++ Implementation	  ****************/
/*********************************************************/

using namespace unum::ukv;
using namespace unum;

ukv_key_t ukv_default_edge_id_k = std::numeric_limits<ukv_key_t>::max();
ukv_vertex_degree_t ukv_vertex_degree_missing_k = std::numeric_limits<ukv_vertex_degree_t>::max();

constexpr std::size_t bytes_in_degrees_header_k = 2 * sizeof(ukv_vertex_degree_t);

inline range_gt<neighborship_t const*> neighbors(ukv_vertex_degree_t const* degrees,
                                                 ukv_key_t const* neighborships,
                                                 ukv_vertex_role_t role = ukv_vertex_role_any_k) {
    auto ships = reinterpret_cast<neighborship_t const*>(neighborships);

    switch (role) {
    case ukv_vertex_source_k: return {ships, ships + degrees[0]};
    case ukv_vertex_target_k: return {ships + degrees[0], ships + degrees[0] + degrees[1]};
    case ukv_vertex_role_any_k: return {ships, ships + degrees[0] + degrees[1]};
    case ukv_vertex_role_unknown_k: return {};
    }
    __builtin_unreachable();
}

inline range_gt<neighborship_t const*> neighbors(value_view_t bytes, ukv_vertex_role_t role = ukv_vertex_role_any_k) {
    // Handle missing vertices
    if (bytes.size() < 2 * sizeof(ukv_vertex_degree_t))
        return {};

    auto degrees = reinterpret_cast<ukv_vertex_degree_t const*>(bytes.begin());
    return neighbors(degrees, reinterpret_cast<ukv_key_t const*>(degrees + 2), role);
}

struct neighborhood_t {
    ukv_key_t center = 0;
    range_gt<neighborship_t const*> targets;
    range_gt<neighborship_t const*> sources;

    neighborhood_t() = default;
    neighborhood_t(neighborhood_t const&) = default;
    neighborhood_t(neighborhood_t&&) = default;

    /**
     * @brief Parses the a single `value_view_t` chunk
     * from the output of `ukv_graph_find_edges`.
     */
    inline neighborhood_t(ukv_key_t center_vertex, value_view_t bytes) noexcept {
        center = center_vertex;
        targets = neighbors(bytes, ukv_vertex_source_k);
        sources = neighbors(bytes, ukv_vertex_target_k);
    }

    inline neighborhood_t(ukv_key_t center_vertex,
                          ukv_vertex_degree_t const* degrees,
                          ukv_key_t const* neighborships) noexcept {
        center = center_vertex;
        targets = neighbors(degrees, neighborships, ukv_vertex_source_k);
        sources = neighbors(degrees, neighborships, ukv_vertex_target_k);
    }

    inline std::size_t size() const noexcept { return targets.size() + sources.size(); }

    inline edge_t operator[](std::size_t i) const noexcept {
        edge_t result;
        if (i > targets.size()) {
            i -= targets.size();
            result.source_id = center;
            result.target_id = targets[i].neighbor_id;
            result.edge_id = targets[i].edge_id;
        }
        else {
            result.source_id = sources[i].neighbor_id;
            result.target_id = center;
            result.edge_id = sources[i].edge_id;
        }
        return result;
    }

    inline edges_view_t outgoing_edges() const& {
        edges_view_t edges;
        edges.source_ids = {&center, 0, targets.size()};
        edges.target_ids = targets.strided().members(&neighborship_t::neighbor_id);
        edges.edge_ids = targets.strided().members(&neighborship_t::edge_id);
        return edges;
    }

    inline edges_view_t incoming_edges() const& {
        edges_view_t edges;
        edges.source_ids = sources.strided().members(&neighborship_t::neighbor_id);
        edges.target_ids = {&center, 0, sources.size()};
        edges.edge_ids = sources.strided().members(&neighborship_t::edge_id);
        return edges;
    }

    inline range_gt<neighborship_t const*> outgoing_to(ukv_key_t target) const noexcept {
        return equal_subrange(targets, target);
    }

    inline range_gt<neighborship_t const*> incoming_from(ukv_key_t source) const noexcept {
        return equal_subrange(sources, source);
    }

    inline neighborship_t const* outgoing_to(ukv_key_t target, ukv_key_t edge_id) const noexcept {
        auto r = equal_subrange(targets, neighborship_t {target, edge_id});
        return r.size() ? r.begin() : nullptr;
    }

    inline neighborship_t const* incoming_from(ukv_key_t source, ukv_key_t edge_id) const noexcept {
        auto r = equal_subrange(sources, neighborship_t {source, edge_id});
        return r.size() ? r.begin() : nullptr;
    }

    inline range_gt<neighborship_t const*> only(ukv_vertex_role_t role) const noexcept {
        switch (role) {
        case ukv_vertex_source_k: return targets;
        case ukv_vertex_target_k: return sources;
        default: return {};
        }
    }

    /**
     * @return true  If the node is present in the graph.
     *               The neighborhood may be empy.
     */
    inline operator bool() const noexcept { return sources && targets; }
};

struct neighborhoods_iterator_t {
    strided_ptr_gt<ukv_key_t const> centers_;
    ukv_vertex_degree_t const* degrees_per_vertex_ = nullptr;
    ukv_key_t const* neighborships_per_vertex_ = nullptr;

    neighborhoods_iterator_t(strided_ptr_gt<ukv_key_t const> centers,
                             ukv_vertex_degree_t const* degrees_per_vertex,
                             ukv_key_t const* neighborships_per_vertex) noexcept
        : centers_(centers), degrees_per_vertex_(degrees_per_vertex),
          neighborships_per_vertex_(neighborships_per_vertex) {}

    inline neighborhood_t operator*() const noexcept {
        return {*centers_, degrees_per_vertex_, neighborships_per_vertex_};
    }
    inline neighborhoods_iterator_t operator++(int) const noexcept {
        return {
            centers_++,
            degrees_per_vertex_ + 2u,
            neighborships_per_vertex_ + (degrees_per_vertex_[0] + degrees_per_vertex_[1]) * 2u,
        };
    }

    inline neighborhoods_iterator_t& operator++() noexcept {
        ++centers_;
        neighborships_per_vertex_ += (degrees_per_vertex_[0] + degrees_per_vertex_[1]) * 2u;
        degrees_per_vertex_ += 2u;
        return *this;
    }

    inline bool operator==(neighborhoods_iterator_t const& other) const noexcept { return centers_ == other.centers_; }
    inline bool operator!=(neighborhoods_iterator_t const& other) const noexcept { return centers_ != other.centers_; }
};

struct neighborhoods_t {
    strided_range_gt<ukv_key_t const> centers_;
    ukv_vertex_degree_t const* degrees_per_vertex_ = nullptr;
    ukv_key_t const* neighborships_per_vertex_ = nullptr;

    neighborhoods_t(strided_range_gt<ukv_key_t const> centers,
                    ukv_vertex_degree_t const* degrees_per_vertex,
                    ukv_key_t const* neighborships_per_vertex) noexcept
        : centers_(centers), degrees_per_vertex_(degrees_per_vertex),
          neighborships_per_vertex_(neighborships_per_vertex) {}

    inline neighborhoods_iterator_t begin() const noexcept {
        return {centers_.begin(), degrees_per_vertex_, neighborships_per_vertex_};
    }
    inline neighborhoods_iterator_t end() const noexcept {
        return {centers_.end(), degrees_per_vertex_ + centers_.size() * 2u, nullptr};
    }
    inline std::size_t size() const noexcept { return centers_.size(); }
};

/**
 * @return true  If such an entry didn't exist and was added.
 * @return false In every other case.
 */
bool upsert(value_t& value, ukv_vertex_role_t role, ukv_key_t neighbor_id, ukv_key_t edge_id) {

    auto ship = neighborship_t {neighbor_id, edge_id};
    if (!value.size()) {
        value = value_t(sizeof(ukv_vertex_degree_t) * 2 + sizeof(neighborship_t));
        auto degrees = reinterpret_cast<ukv_vertex_degree_t*>(value.begin());
        auto ships = reinterpret_cast<neighborship_t*>(degrees + 2);
        degrees[role != ukv_vertex_target_k] = 0;
        degrees[role == ukv_vertex_target_k] = 1;
        ships[0] = ship;
        return true;
    }

    auto neighbors_range = neighbors(value, role);
    auto it = std::lower_bound(neighbors_range.begin(), neighbors_range.end(), ship);
    if (it != neighbors_range.end())
        if (*it == ship)
            return false;

    auto off = reinterpret_cast<byte_t const*>(it) - value.begin();
    auto ship_bytes = reinterpret_cast<byte_t const*>(&ship);
    value.insert(off, ship_bytes, ship_bytes + sizeof(neighborship_t));
    auto degrees = reinterpret_cast<ukv_vertex_degree_t*>(value.begin());
    degrees[role == ukv_vertex_target_k] += 1;
    return true;
}

/**
 * @return true  If such a atching entry was found and deleted.
 * @return false In every other case.
 */
bool erase(value_t& value, ukv_vertex_role_t role, ukv_key_t neighbor_id, std::optional<ukv_key_t> edge_id = {}) {

    if (!value.size())
        return false;

    std::size_t off = 0;
    std::size_t len = 0;

    auto neighbors_range = neighbors(value, role);
    if (edge_id) {
        auto ship = neighborship_t {neighbor_id, *edge_id};
        auto it = std::lower_bound(neighbors_range.begin(), neighbors_range.end(), ship);
        if (it != neighbors_range.end())
            if (*it != ship)
                return false;

        off = reinterpret_cast<byte_t const*>(it) - value.begin();
        len = sizeof(neighborship_t);
    }
    else {
        auto pair = std::equal_range(neighbors_range.begin(), neighbors_range.end(), neighbor_id);
        if (pair.first == pair.second)
            return false;

        off = reinterpret_cast<byte_t const*>(pair.first) - value.begin();
        len = sizeof(neighborship_t) * (pair.second - pair.first);
    }

    value.erase(off, len);
    auto degrees = reinterpret_cast<ukv_vertex_degree_t*>(value.begin());
    degrees[role == ukv_vertex_target_k] -= 1;
    return true;
}

template <bool export_center_ak = true, bool export_neighbor_ak = true, bool export_edge_ak = true>
void export_edge_tuples( //
    ukv_t const c_db,
    ukv_txn_t const c_txn,

    ukv_collection_t const* c_collections,
    ukv_size_t const c_collections_stride,

    ukv_key_t const* c_vertices_ids,
    ukv_size_t const c_vertices_count,
    ukv_size_t const c_vertices_stride,

    ukv_vertex_role_t const* c_roles,
    ukv_size_t const c_roles_stride,

    ukv_options_t const c_options,

    ukv_vertex_degree_t** c_degrees_per_vertex,
    ukv_key_t** c_neighborships_per_vertex,

    ukv_arena_t* c_arena,
    ukv_error_t* c_error) {

    ukv_val_len_t* c_found_lengths = nullptr;
    ukv_val_ptr_t c_found_values = nullptr;

    ukv_read(c_db,
             c_txn,
             c_collections,
             c_collections_stride,
             c_vertices_ids,
             c_vertices_count,
             c_vertices_stride,
             c_options,
             &c_found_lengths,
             &c_found_values,
             c_arena,
             c_error);
    if (*c_error)
        return;

    stl_arena_t& arena = *cast_arena(c_arena, c_error);
    if (*c_error)
        return;

    taped_values_view_t values {c_found_lengths, c_found_values, c_vertices_count};
    strided_range_gt<ukv_key_t const> vertices_ids {c_vertices_ids, c_vertices_stride, c_vertices_count};
    strided_ptr_gt<ukv_vertex_role_t const> roles {c_roles, c_roles_stride};

    // Estimate the amount of memory we will need for the arena
    std::size_t total_neighborships = 0;
    {
        tape_iterator_t values_it = values.begin();
        for (ukv_size_t i = 0; i != c_vertices_count; ++i, ++values_it) {
            value_view_t value = *values_it;
            ukv_vertex_role_t role = roles[i];
            total_neighborships += neighbors(value, role).size();
        }
    }
    constexpr std::size_t tuple_size_k = export_center_ak + export_neighbor_ak + export_edge_ak;
    arena.unpacked_tape.resize(total_neighborships * sizeof(ukv_key_t) * tuple_size_k +
                               c_vertices_count * sizeof(ukv_vertex_degree_t));

    // Export into arena
    auto const degrees_per_vertex = reinterpret_cast<ukv_vertex_degree_t*>(arena.unpacked_tape.data());
    auto neighborships_per_vertex = reinterpret_cast<ukv_key_t*>(degrees_per_vertex + c_vertices_count);

    tape_iterator_t values_it = values.begin();
    for (ukv_size_t i = 0; i != c_vertices_count; ++i, ++values_it) {
        value_view_t value = *values_it;
        ukv_key_t vertex_id = vertices_ids[i];
        ukv_vertex_role_t role = roles[i];
        ukv_vertex_degree_t& degree = degrees_per_vertex[i];

        // Some values may be missing
        if (value.empty()) {
            degree = ukv_vertex_degree_missing_k;
            continue;
        }

        degree = 0;
        if (role & ukv_vertex_source_k) {
            auto ns = neighbors(value, ukv_vertex_source_k);
            if constexpr (tuple_size_k)
                for (neighborship_t n : ns) {
                    if constexpr (export_center_ak)
                        neighborships_per_vertex[0] = vertex_id;
                    if constexpr (export_neighbor_ak)
                        neighborships_per_vertex[export_center_ak] = n.neighbor_id;
                    if constexpr (export_edge_ak)
                        neighborships_per_vertex[export_center_ak + export_neighbor_ak] = n.edge_id;
                    neighborships_per_vertex += tuple_size_k;
                }
            degree += static_cast<ukv_vertex_degree_t>(ns.size());
        }
        if (role & ukv_vertex_target_k) {
            auto ns = neighbors(value, ukv_vertex_target_k);
            if constexpr (tuple_size_k)
                for (neighborship_t n : ns) {
                    if constexpr (export_neighbor_ak)
                        neighborships_per_vertex[0] = n.neighbor_id;
                    if constexpr (export_center_ak)
                        neighborships_per_vertex[export_neighbor_ak] = vertex_id;
                    if constexpr (export_edge_ak)
                        neighborships_per_vertex[export_center_ak + export_neighbor_ak] = n.edge_id;
                    neighborships_per_vertex += tuple_size_k;
                }
            degree += static_cast<ukv_vertex_degree_t>(ns.size());
        }
    }

    *c_degrees_per_vertex = reinterpret_cast<ukv_vertex_degree_t*>(arena.unpacked_tape.data());
    *c_neighborships_per_vertex = reinterpret_cast<ukv_key_t*>(degrees_per_vertex + c_vertices_count);
}

void export_disjoint_edge_buffers( //
    ukv_t const c_db,
    ukv_txn_t const c_txn,

    ukv_collection_t const* c_collections,
    ukv_size_t const c_collections_stride,

    ukv_key_t const* c_vertices_ids,
    ukv_size_t const c_vertices_count,
    ukv_size_t const c_vertices_stride,

    ukv_options_t const c_options,

    ukv_arena_t* c_arena,
    ukv_error_t* c_error) {

    ukv_val_len_t* c_found_lengths = nullptr;
    ukv_val_ptr_t c_found_values = nullptr;

    ukv_read(c_db,
             c_txn,
             c_collections,
             c_collections_stride,
             c_vertices_ids,
             c_vertices_count,
             c_vertices_stride,
             c_options,
             &c_found_lengths,
             &c_found_values,
             c_arena,
             c_error);
    if (*c_error)
        return;

    stl_arena_t& arena = *cast_arena(c_arena, c_error);
    if (*c_error)
        return;

    taped_values_view_t values {c_found_lengths, c_found_values, c_vertices_count};
    for (value_view_t value : values)
        arena.updated_vals.emplace_back(value);
}

template <bool erase_ak>
void update_neighborhoods( //
    ukv_t const c_db,
    ukv_txn_t const c_txn,

    ukv_collection_t const* c_collections,
    ukv_size_t const c_collections_stride,

    ukv_key_t const* c_edges_ids,
    ukv_size_t const c_edges_count,
    ukv_size_t const c_edges_stride,

    ukv_key_t const* c_sources_ids,
    ukv_size_t const c_sources_stride,

    ukv_key_t const* c_targets_ids,
    ukv_size_t const c_targets_stride,

    ukv_options_t const c_options,

    ukv_arena_t* c_arena,
    ukv_error_t* c_error) {

    stl_arena_t& arena = *cast_arena(c_arena, c_error);
    if (*c_error)
        return;

    strided_ptr_gt<ukv_collection_t const> collections {c_collections, c_collections_stride};
    strided_ptr_gt<ukv_key_t const> edges_ids {c_edges_ids, c_edges_stride};
    strided_ptr_gt<ukv_key_t const> sources_ids {c_sources_ids, c_sources_stride};
    strided_ptr_gt<ukv_key_t const> targets_ids {c_targets_ids, c_targets_stride};

    // Fetch all the data related to touched vertices
    arena.updated_keys.resize(c_edges_count + c_edges_count);
    for (ukv_size_t i = 0; i != c_edges_count; ++i)
        arena.updated_keys[i] = {collections[i], sources_ids[i]};
    for (ukv_size_t i = 0; i != c_edges_count; ++i)
        arena.updated_keys[c_edges_count + i] = {collections[i], targets_ids[i]};

    // Keep only the unique items
    sort_and_deduplicate(arena.updated_keys);

    export_disjoint_edge_buffers(c_db,
                                 c_txn,
                                 &arena.updated_keys[0].collection,
                                 sizeof(located_key_t),
                                 &arena.updated_keys[0].key,
                                 static_cast<ukv_size_t>(arena.updated_keys.size()),
                                 sizeof(located_key_t),
                                 c_options,
                                 c_arena,
                                 c_error);
    if (*c_error)
        return;

    // Upsert into in-memory arrays
    for (ukv_size_t i = 0; i != c_edges_count; ++i) {
        auto collection = collections[i];
        auto source_id = sources_ids[i];
        auto target_id = targets_ids[i];

        auto& source_value = arena.updated_vals[offset_in_sorted(arena.updated_keys, {collection, source_id})];
        auto& target_value = arena.updated_vals[offset_in_sorted(arena.updated_keys, {collection, target_id})];

        if (erase_ak) {
            std::optional<ukv_key_t> edge_id;
            if (edges_ids)
                edge_id = edges_ids[i];

            erase(source_value, ukv_vertex_source_k, target_id, edge_id);
            erase(target_value, ukv_vertex_target_k, source_id, edge_id);
        }
        else {
            auto edge_id = edges_ids[i];
            upsert(source_value, ukv_vertex_source_k, target_id, edge_id);
            upsert(target_value, ukv_vertex_target_k, source_id, edge_id);
        }
    }

    // Dump the data back to disk!
    ukv_val_len_t offset_in_val = 0;
    ukv_write(c_db,
              c_txn,
              &arena.updated_keys[0].collection,
              sizeof(located_key_t),
              &arena.updated_keys[0].key,
              static_cast<ukv_size_t>(arena.updated_keys.size()),
              sizeof(located_key_t),
              arena.updated_vals[0].internal_cptr(),
              sizeof(value_t),
              &offset_in_val,
              0,
              arena.updated_vals[0].internal_length(),
              sizeof(value_t),
              c_options,
              c_arena,
              c_error);
}

void ukv_graph_find_edges( //
    ukv_t const c_db,
    ukv_txn_t const c_txn,

    ukv_collection_t const* c_collections,
    ukv_size_t const c_collections_stride,

    ukv_key_t const* c_vertices_ids,
    ukv_size_t const c_vertices_count,
    ukv_size_t const c_vertices_stride,

    ukv_vertex_role_t const* c_roles,
    ukv_size_t const c_roles_stride,

    ukv_options_t const c_options,

    ukv_vertex_degree_t** c_degrees_per_vertex,
    ukv_key_t** c_neighborships_per_vertex,

    ukv_arena_t* c_arena,
    ukv_error_t* c_error) {

    auto func = (c_options & ukv_option_read_lengths_k) ? &export_edge_tuples<false, false, false>
                                                        : &export_edge_tuples<true, true, true>;
    return func(c_db,
                c_txn,
                c_collections,
                c_collections_stride,
                c_vertices_ids,
                c_vertices_count,
                c_vertices_stride,
                c_roles,
                c_roles_stride,
                c_options,
                c_degrees_per_vertex,
                c_neighborships_per_vertex,
                c_arena,
                c_error);
}

void ukv_graph_upsert_edges( //
    ukv_t const c_db,
    ukv_txn_t const c_txn,

    ukv_collection_t const* c_collections,
    ukv_size_t const c_collections_stride,

    ukv_key_t const* c_edges_ids,
    ukv_size_t const c_edges_count,
    ukv_size_t const c_edges_stride,

    ukv_key_t const* c_sources_ids,
    ukv_size_t const c_sources_stride,

    ukv_key_t const* c_targets_ids,
    ukv_size_t const c_targets_stride,

    ukv_options_t const c_options,

    ukv_arena_t* c_arena,
    ukv_error_t* c_error) {

    return update_neighborhoods<false>(c_db,
                                       c_txn,
                                       c_collections,
                                       c_collections_stride,
                                       c_edges_ids,
                                       c_edges_count,
                                       c_edges_stride,
                                       c_sources_ids,
                                       c_sources_stride,
                                       c_targets_ids,
                                       c_targets_stride,
                                       c_options,
                                       c_arena,
                                       c_error);
}

void ukv_graph_remove_edges( //
    ukv_t const c_db,
    ukv_txn_t const c_txn,

    ukv_collection_t const* c_collections,
    ukv_size_t const c_collections_stride,

    ukv_key_t const* c_edges_ids,
    ukv_size_t const c_edges_count,
    ukv_size_t const c_edges_stride,

    ukv_key_t const* c_sources_ids,
    ukv_size_t const c_sources_stride,

    ukv_key_t const* c_targets_ids,
    ukv_size_t const c_targets_stride,

    ukv_options_t const c_options,

    ukv_arena_t* c_arena,
    ukv_error_t* c_error) {

    return update_neighborhoods<true>(c_db,
                                      c_txn,
                                      c_collections,
                                      c_collections_stride,
                                      c_edges_ids,
                                      c_edges_count,
                                      c_edges_stride,
                                      c_sources_ids,
                                      c_sources_stride,
                                      c_targets_ids,
                                      c_targets_stride,
                                      c_options,
                                      c_arena,
                                      c_error);
}

void ukv_graph_remove_vertices( //
    ukv_t const c_db,
    ukv_txn_t const c_txn,

    ukv_collection_t const* c_collections,
    ukv_size_t const c_collections_stride,

    ukv_key_t const* c_vertices_ids,
    ukv_size_t const c_vertices_count,
    ukv_size_t const c_vertices_stride,

    ukv_vertex_role_t const* c_roles,
    ukv_size_t const c_roles_stride,

    ukv_options_t const c_options,

    ukv_arena_t* c_arena,
    ukv_error_t* c_error) {

    strided_ptr_gt<ukv_collection_t const> collections {c_collections, c_collections_stride};
    strided_range_gt<ukv_key_t const> vertices_ids {c_vertices_ids, c_vertices_stride, c_vertices_count};
    strided_ptr_gt<ukv_vertex_role_t const> roles {c_roles, c_roles_stride};

    // Initially, just retrieve the bare minimum information about the vertices
    ukv_vertex_degree_t* degrees_per_vertex = nullptr;
    ukv_key_t* neighbors_per_vertex = nullptr;
    export_edge_tuples<false, true, false>(c_db,
                                           c_txn,
                                           c_collections,
                                           c_collections_stride,
                                           c_vertices_ids,
                                           c_vertices_count,
                                           c_vertices_stride,
                                           c_roles,
                                           c_roles_stride,
                                           c_options,
                                           &degrees_per_vertex,
                                           &neighbors_per_vertex,
                                           c_arena,
                                           c_error);

    if (*c_error)
        return;

    stl_arena_t& arena = *cast_arena(c_arena, c_error);
    if (*c_error)
        return;

    // Enumerate the opposite ends, from which that same reference must be removed.
    // Here all the keys will be in the sorted order.
    std::size_t count_edges = std::accumulate(degrees_per_vertex, degrees_per_vertex + c_vertices_count, 0ul);
    arena.updated_keys.reserve(count_edges * 2);
    for (ukv_size_t i = 0; i != c_vertices_count; ++i, ++degrees_per_vertex) {
        auto collection = collections[i];
        for (ukv_size_t j = 0; j != *degrees_per_vertex; ++i, ++neighbors_per_vertex)
            arena.updated_keys.push_back({collection, *neighbors_per_vertex});
    }

    // Sorting the tasks would help us faster locate them in the future.
    // We may also face repetitions when connected vertices are removed.
    sort_and_deduplicate(arena.updated_keys);
    arena.updated_vals.resize(arena.updated_keys.size());

    // Fetch the opposite ends, from which that same reference must be removed.
    // Here all the keys will be in the sorted order.
    export_disjoint_edge_buffers(c_db,
                                 c_txn,
                                 &arena.updated_keys[0].collection,
                                 sizeof(located_key_t),
                                 &arena.updated_keys[0].key,
                                 static_cast<ukv_size_t>(arena.updated_keys.size()),
                                 sizeof(located_key_t),
                                 c_options,
                                 c_arena,
                                 c_error);
    if (c_error)
        return;

    // From every sequence remove the matching range of neighbors
    for (ukv_size_t i = 0; i != arena.updated_keys.size(); ++i) {
        auto collection = collections[i];
        auto vertex_id = vertices_ids[i];
        auto role = roles[i];

        auto vertex_idx = offset_in_sorted(arena.updated_keys, {collection, vertex_id});
        value_t& vertex_value = arena.updated_vals[vertex_idx];

        for (neighborship_t n : neighbors(vertex_value, role)) {
            auto neighbor_idx = offset_in_sorted(arena.updated_keys, {collection, n.neighbor_id});
            value_t& neighbor_value = arena.updated_vals[neighbor_idx];
            erase(neighbor_value, invert(role), vertex_id);
        }

        vertex_value.clear();
    }

    // Now we will go through all the explicitly deleted vertices
    ukv_val_len_t offset_in_val = 0;
    ukv_write(c_db,
              c_txn,
              &arena.updated_keys[0].collection,
              sizeof(located_key_t),
              &arena.updated_keys[0].key,
              static_cast<ukv_size_t>(arena.updated_keys.size()),
              sizeof(located_key_t),
              arena.updated_vals[0].internal_cptr(),
              sizeof(value_t),
              &offset_in_val,
              0,
              arena.updated_vals[0].internal_length(),
              sizeof(value_t),
              c_options,
              c_arena,
              c_error);
}