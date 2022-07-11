/**
 * @file test.cpp
 * @author Ashot Vardanian
 * @date 2022-07-06
 *
 * @brief A set of tests implemented using Google Test.
 */

#include <gtest/gtest.h>

#include "ukv.hpp"
#include "ukv_graph.hpp"
#include "ukv_docs.hpp"

using namespace unum::ukv;
using namespace unum;

void round_trip(sample_proxy_t proxy, disjoint_values_view_t values) {

    EXPECT_FALSE(proxy.set(values)) << "Failed to assign";

    EXPECT_TRUE(proxy.get()) << "Failed to fetch inserted keys";

    // Validate that values match
    taped_values_view_t retrieved = *proxy.get();
    EXPECT_EQ(retrieved.size(), proxy.keys.size());
    tape_iterator_t it = retrieved.begin();
    for (std::size_t i = 0; i != proxy.keys.size(); ++i, ++it) {
        auto expected_len = static_cast<std::size_t>(values.lengths[i]);
        auto expected_begin = reinterpret_cast<byte_t const*>(values.contents[i]) + values.offsets[i];

        value_view_t val_view = *it;
        EXPECT_EQ(val_view.size(), expected_len);
        EXPECT_TRUE(std::equal(val_view.begin(), val_view.end(), expected_begin));
    }
}

TEST(db, basic) {

    db_t db;
    EXPECT_FALSE(db.open(""));

    session_t session = db.session();

    std::vector<ukv_key_t> keys {34, 35, 36};
    ukv_val_len_t val_len = sizeof(std::uint64_t);
    std::vector<std::uint64_t> vals {34, 35, 36};
    std::vector<ukv_val_len_t> offs {0, val_len, val_len * 2};
    auto vals_begin = reinterpret_cast<ukv_val_ptr_t>(vals.data());

    sample_proxy_t proxy = session[keys];
    disjoint_values_view_t values {
        .contents = {&vals_begin, 0, 3},
        .offsets = offs,
        .lengths = {val_len, 3},
    };
    round_trip(proxy, values);

    // Overwrite those values with same size integers and try again
    for (auto& val : vals)
        val += 100;
    round_trip(proxy, values);

    // TODO: Add tests for empty values
}

TEST(db, net) {

    db_t db;
    EXPECT_FALSE(db.open(""));

    graph_collection_session_t net {collection_t(db)};

    std::vector<edge_t> triangle {
        {1, 2, 9},
        {2, 3, 10},
        {3, 1, 11},
    };

    EXPECT_FALSE(net.upsert({triangle}));

    EXPECT_TRUE(net.edges(1));

    EXPECT_EQ(net.edges(1)->size(), 2ul);
    EXPECT_EQ(net.edges(1, ukv_vertex_source_k)->size(), 1ul);
    EXPECT_EQ(net.edges(1, ukv_vertex_target_k)->size(), 1ul);

    EXPECT_EQ(net.edges(3, ukv_vertex_target_k)->size(), 1ul);
    EXPECT_EQ(net.edges(2, ukv_vertex_source_k)->size(), 1ul);
    EXPECT_EQ(net.edges(3, 1)->size(), 1ul);
    EXPECT_EQ(net.edges(1, 3)->size(), 0ul);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}