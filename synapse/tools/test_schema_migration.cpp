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
    std::vector<uint8_t> data(text.begin(), text.end());
    return data;
}

static std::string payload_as_string(const std::vector<uint8_t>& data) {
    return std::string(data.begin(), data.end());
}

int main() {
    SchemaMigration schema;

    // 1) Register a v0->v1 migration that appends a marker byte.
    schema.register_migration(0, 1, [](const std::vector<uint8_t>& data) {
        std::vector<uint8_t> out(data);
        out.push_back('v');
        return out;
    });

    // 2) Compat: v1 reader should read v1 data.
    assert(schema.is_compatible(1, 1) == true);
    // 3) Backward compat: v1 reader should read v0 data for +1 rule.
    assert(schema.is_compatible(1, 0) == true);
    assert(schema.is_compatible(1, 2) == true);
    assert(schema.is_compatible(1, 3) == false);
    assert(schema.is_compatible(1, 0) == true);

    // 4) v0 raw bytes -> v1 via migration.
    auto migrated = schema.migrate(make_payload("hello"), 0, 1);
    assert(payload_as_string(migrated) == "hellov");

    // 5) v1 -> v1 returns identity.
    auto identity = schema.migrate(make_payload("world"), 1, 1);
    assert(payload_as_string(identity) == "world");

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
    assert(schema.has_path(1, 0) == false);

    std::cout << "Result: PASS\n";
    return 0;
}
