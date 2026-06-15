// SHA-256 checksums keyed by version then platform.
// Updated by CI alongside lua/lazyverilog/checksums.lua.
export const RELEASE_CHECKSUMS: Record<string, Record<string, string>> = {
  "v1.3.2": {
    "linux-x64": "b0a62f6a81c4304eed526529ec6e2d8191a7d2a88ca49cc7fe5e3b45bb4fe430",
    "linux-arm64": "ba6352166b072e11c569f3b866629e59c00a1914dbff238c4ebe0a78471ea7f2",
    "linux-x64-static": "1328f9282effcd6af3ac7f6e5cb2227b0369036e4490e015cd5d808244f39109",
    "linux-arm64-static": "281ab000359b8fd41ee9cd67e21a76e3f2e576ed145226f830361fa9b72036f8",
    "darwin-x64": "8ea3887ac0d2826e70e7ac325f69d19b4b59b31a2e7895ce2006ec316abee26e",
    "darwin-arm64": "2bd744ad14d61b8baa881908504cc1b6c425cc1aa5201b273ec9fe20e357e905",
    "windows-x64": "61523b1c73d8098cd6f2f53cc1e6d28abb2abc4605bbc9b4a83761f0b5a128ad",
  },
};
