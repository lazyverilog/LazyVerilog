// SHA-256 checksums keyed by version then platform.
// Updated by CI alongside lua/lazyverilog/checksums.lua.
export const RELEASE_CHECKSUMS: Record<string, Record<string, string>> = {
  "v2.1.0": {
    "linux-x64": "46aedf66e50d2dd36bd79cab559ffbc0fe3c44a63d9d5467cf8f0b5eea20ff3f",
    "linux-arm64": "8bc3cc8b21b3f38eb73d0db1013312fcfb0d9907e3dca1896810f1da6cf78385",
    "linux-x64-static": "93c6ecebf2983f82b567c99b1a8f4cc7722e3432be3fe872026aa96b91e1a3fb",
    "linux-arm64-static": "155460b52f99db5fca359942d6b635780df0562f620086e9634f4c5e9be01216",
    "darwin-x64": "c57bb8345d4e2092d82158e1e28a2f95d731c544bc5041cfd39c6c2dc2727f7d",
    "darwin-arm64": "95eca7d58daeb170c7570f97781501d828560b3f6589c7d532b4a41ea125f398",
    "windows-x64": "fadde16481954774aab8b3883ad6570ff9befac959fc7514f757cf9ea1c47f07",
  },
};
