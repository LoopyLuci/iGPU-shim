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

    std::cout << "Result: PASS\n";
    return 0;
}
