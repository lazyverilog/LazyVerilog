// SHA-256 checksums keyed by version then platform.
// Updated by CI alongside lua/lazyverilog/checksums.lua.
export const RELEASE_CHECKSUMS: Record<string, Record<string, string>> = {
  "v2.2.0": {
    "linux-x64": "69b812e6906a09797e16aa6015a72c84cadd6847b41f5c6da0201433a0de7dba",
    "linux-arm64": "44e5bee11e40782d0a30baf017526f82b77dc3e38d113715fad19e8fc2b7b699",
    "linux-x64-static": "3ecd8a6248943bf2131d7848799b58c67bc17718555f79c5fcfdeb5765f4f8b3",
    "linux-arm64-static": "5c956081b98206da7a74cf230dc8c1ee116a1868a3bfeb2d2bbec8ad63d79e95",
    "darwin-x64": "298ef519b25cf8f04dff40b01d9b6fcf3956a43c7f8864bb2e5f9bad995d2401",
    "darwin-arm64": "b65e3676c0e066574bd73223fa14b5019e514f92482367c5ca16131fdc3e1afa",
    "windows-x64": "46c076d6359438d9831bf4ac3f98f56203a3486e8beb474ab8fa398977d3763b",
  },
};
