// SHA-256 checksums keyed by version then platform.
// Updated by CI alongside lua/lazyverilog/checksums.lua.
export const RELEASE_CHECKSUMS: Record<string, Record<string, string>> = {
  "v2.1.1": {
    "linux-x64": "2ab9ec51e5e9b63ac322c176545e4734c0e6760f14d00e6adc5579269a2484b7",
    "linux-arm64": "84a225f5d29e1d0eae95f58eb28e2d2ca4a543ee31d7ab74d61e247ba18e93ce",
    "linux-x64-static": "9ffffa803fcbdebff781fbd7e23b662fef13eb398b54ea6ded022f61b0045d27",
    "linux-arm64-static": "bb3546f9251f57bc4de9ccd8224cc96557027ee777e3e4afa3622ce850579331",
    "darwin-x64": "0b3d9fc48409a48bac5f3bf59939e34364ee7ddd53ac3b98e5fff9a312fd55de",
    "darwin-arm64": "d9aa9cb74807bdaba9bcbdd23e78f9eb56917d9c20e79c8d70d4053c0fd1e5e0",
    "windows-x64": "c80525b2bf311ac0c31f3bec9c12628ec1934f24134973179dbacffb4826899a",
  },
};
