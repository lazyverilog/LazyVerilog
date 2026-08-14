// SHA-256 checksums keyed by version then platform.
// Updated by CI alongside lua/lazyverilog/checksums.lua.
export const RELEASE_CHECKSUMS: Record<string, Record<string, string>> = {
  "v1.3.6": {
    "linux-x64": "e0129ad0c52d5f95db40fb9f9d94744487a0b81b0033c7b4f56a280cbfff1652",
    "linux-arm64": "e801e526a7886369f87f05c9a155927652b3693377e89d387bc58a251679d1ea",
    "linux-x64-static": "5b7a25b73ac36eeda0d5e29f2a199eedd0aefe4a3b645407c34ec3eb1f0d35b0",
    "linux-arm64-static": "119c3131741922f4487e8c064dbd3bfe95a79465de805ff5cf50c87c1a9f16d7",
    "darwin-x64": "de4b7a6cbd4899a2aa991c082624527d8663b7359b6fc430a5cb62ec86404994",
    "darwin-arm64": "6ec5122aa2c9b2804ef3f355b277a4f7400598386f3fda8c55cfa7a7b4dba3c6",
    "windows-x64": "69fb2108cf9920a0e679a0dcad856aece870ff55cd193c8b77b6c5571aff161a",
  },
};
