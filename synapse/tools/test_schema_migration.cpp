/**
 * @file test_schema_migration.cpp
 * @brief Unit tests for schema versioning and migration scaffolding.
 */

#include "../protocol/schema_migration.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace synapse::protocol {

std::ostream& operator<<(std::ostream& os, const MessageHeader& hdr) {
    return os << "MessageHeader{magic=" << hdr.magic
              << ", version=" << hdr.schema_version
              << ", size=" << hdr.payload_size << "}";
}

}  // namespace synapse::protocol

using namespace synapse::protocol;

static std::vector<uint8_t> make_payload(const std::string& text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

int main() {
    SchemaMigration schema;

    // 1) Register a v0->v1 migration that appends a marker byte.
    schema.register_migration(0, 1, [](const std::vector<uint8_t>& data) {
        std::vector<uint8_t> out(data);
        out.push_back('v');
        return out;
    });
    schema.register_migration(1, 2, [](const std::vector<uint8_t>& data) {
        std::vector<uint8_t> out(data);
        out.push_back('2');
        return out;
    });

    // 2) Compat checks.
    assert(schema.is_compatible(1, 1) == true);
    assert(schema.is_compatible(1, 0) == true);
    assert(schema.is_compatible(1, 2) == true);
    assert(schema.is_compatible(1, 3) == false);

    // 3) v0 raw bytes -> v1 via migration.
    auto migrated = schema.migrate(make_payload("hello"), 0, 1);
    assert(std::string(migrated.begin(), migrated.end()) == "hellov");

    // 4) Multi-hop: v0 -> v2 through v1.
    auto multi = schema.migrate(make_payload("base"), 0, 2);
    assert(payload_as_string(multi) == "basev2");

    // 5) v1 -> v1 returns identity without registered migration.
    auto identity = schema.migrate(make_payload("world"), 1, 1);
    assert(payload_as_string(identity) == "world");

    // 5b) Same-version path discovery is true even without a direct edge.
    assert(schema.has_path(1, 1) == true);

    // 5c) Downgrade with no registered reverse edge throws.
    bool downgrade_threw = false;
    try {
        schema.migrate(make_payload("x"), 2, 0);
    } catch (const std::exception&) {
        downgrade_threw = true;
    }
    assert(downgrade_threw);

    // 5d) Duplicate registration is ignored and does not inflate count.
    schema.register_migration(0, 1, [](const std::vector<uint8_t>& data) {
        return data;
    });
    assert(schema.migration_count() == 2);

    // 6) Missing path throws.
    bool threw = false;
    try {
        schema.migrate(make_payload("x"), 9, 1);
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);

    // 7) Path discovery.
    assert(schema.has_path(0, 1) == true);
    assert(schema.has_path(0, 2) == true);
    assert(schema.has_path(1, 0) == false);
    assert(schema.has_path(2, 9) == false);

    // 8) Registry metadata.
    assert(schema.latest_version() == 2);
    assert(schema.migration_count() == 2);

    // ── Round-trip tests ──────────────────────────────────────────────

    // 9a) Register reverse migrations for round-trip verification.
    SchemaMigration rt_schema;
    rt_schema.register_migration(0, 1, [](const std::vector<uint8_t>& data) {
        std::vector<uint8_t> out(data);
        out.push_back('X');
        return out;
    });
    rt_schema.register_migration(1, 0, [](const std::vector<uint8_t>& data) {
        std::vector<uint8_t> out(data);
        if (!out.empty() && out.back() == 'X') out.pop_back();
        return out;
    });

    // 9b) Round-trip: v0 -> v1 -> v0.
    auto v0_data = make_payload("test_round_trip");
    auto v1_data = rt_schema.migrate(v0_data, 0, 1);
    auto v0_back  = rt_schema.migrate(v1_data, 1, 0);
    assert(v0_data == v0_back && "round-trip v0->v1->v0 should preserve data");

    // 9c) Round-trip: v1 -> v0 -> v1 (starting from already-migrated data).
    auto v1_again = rt_schema.migrate(v0_back, 0, 1);
    assert(v1_again == v1_data && "round-trip v1->v0->v1 should preserve data");

    // ── Fuzz-style path discovery tests ───────────────────────────────

    // 10a) Build a chain 0->1->2->3->4->5 and verify all paths.
    SchemaMigration chain;
    for (uint32_t i = 0; i < 5; ++i) {
        chain.register_migration(i, i + 1, [](const std::vector<uint8_t>& data) {
            return data;
        });
    }

    // Every forward path should exist.
    for (uint32_t from = 0; from < 5; ++from) {
        for (uint32_t to = from + 1; to <= 5; ++to) {
            assert(chain.has_path(from, to) && "forward path should exist");
        }
    }

    // No backward path exists without reverse migrations.
    for (uint32_t to = 0; to < 5; ++to) {
        for (uint32_t from = to + 1; from <= 5; ++from) {
            assert(!chain.has_path(from, to) && "backward path should not exist");
        }
    }

    // 10b) Multi-hop migration: v0 -> v5 (through 1,2,3,4).
    auto hop5 = chain.migrate(make_payload("hop"), 0, 5);
    assert(payload_as_string(hop5) == "hop" && "identity multi-hop should preserve payload");

    // 10c) Self-migration is identity even without an explicit edge.
    auto self = chain.migrate(make_payload("self"), 3, 3);
    assert(payload_as_string(self) == "self");

    // ── Migration composition ─────────────────────────────────────────

    // 11a) Verify multi-hop applies migrations in order.
    SchemaMigration ordered;
    ordered.register_migration(0, 1, [](const std::vector<uint8_t>& d) {
        auto out = d; out.push_back('A'); return out;
    });
    ordered.register_migration(1, 2, [](const std::vector<uint8_t>& d) {
        auto out = d; out.push_back('B'); return out;
    });
    ordered.register_migration(2, 3, [](const std::vector<uint8_t>& d) {
        auto out = d; out.push_back('C'); return out;
    });

    auto composed = ordered.migrate(make_payload("ord"), 0, 3);
    assert(payload_as_string(composed) == "ordABC" && "multi-hop should apply in order");

    std::cout << "Result: PASS\n";
    return 0;
}
