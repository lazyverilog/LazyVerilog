// SHA-256 checksums keyed by version then platform.
// Updated by CI alongside lua/lazyverilog/checksums.lua.
export const RELEASE_CHECKSUMS: Record<string, Record<string, string>> = {
  "v2.2.1": {
    "linux-x64": "48451855dfee48b1c77b7a7d746fbeee98a7b53dda6a6f2e806e491d46b0e4c6",
    "linux-arm64": "93bace6ae9c03f5cebf3a55bf8f5014b7cd4a8250b9f020348b36f8b2da3486c",
    "linux-x64-static": "58575272124e571193a3c17cd864c3f90840611133e102186e6bfd64afb752f7",
    "linux-arm64-static": "1f5ee155505b1cfac7a91b14628ac217625909ad813c79ef3a87e6675e67a843",
    "darwin-x64": "c95624235fadeac24cfb84f4f8401850d50b7e72cb2c03e2688f22f5e74a166e",
    "darwin-arm64": "81e6eb6f6d7600c076b5584a6762035962348d2fa605f2bcace591258e645c9f",
    "windows-x64": "31a8ff05d5eefafa86b88cbd5866686b0514393068c4e47fb56164dba0aae5ed",
  },
};
