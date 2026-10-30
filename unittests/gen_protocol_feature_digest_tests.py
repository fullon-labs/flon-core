#!/usr/bin/env python3

import re
import hashlib
import sys

pattern = re.compile(r"\*\n(Builtin protocol feature: (.*)(?:\*[^/]|[^\*])*)\*/")

# ACTION_DATA_TO_JSON was activated with this description digest before the
# prose/hash mismatch was discovered. Its digest is consensus state and cannot
# be replaced with a newly calculated value on an existing chain.
consensus_description_digest_overrides = {
    "ACTION_DATA_TO_JSON": "980dbdaf3f7113763b62acebfd33b21b71f925f57e64b78bfd932b356190bdb1",
}

def main():
    with open(sys.argv[1], "r") as f:
        contents = f.read()
        print('#include <eosio/chain/protocol_feature_manager.hpp>')
        print('#include <map>')

        print('#include <boost/test/unit_test.hpp>')

        print('using namespace eosio::chain;')

        print('BOOST_AUTO_TEST_CASE(protocol_feature_digest_tests) {')
        print('   std::map<std::string, std::string> digests;')
        for match in re.finditer(pattern, contents):
            codename = match.group(2)
            digest = consensus_description_digest_overrides.get(
                codename, hashlib.sha256(match.group(1).encode('utf8')).hexdigest()
            )
            print('   digests.emplace("%s", "%s");' % (codename, digest))
        print('   for(const auto& [id, spec] : builtin_protocol_feature_codenames) {')
        print('      BOOST_TEST(digests[spec.codename] == fc::variant(spec.description_digest).as<std::string>());')
        print('   }')
        print('}')

if __name__ == "__main__":
    main()
