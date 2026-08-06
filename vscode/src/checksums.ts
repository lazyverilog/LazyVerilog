// SHA-256 checksums keyed by version then platform.
// Updated by CI alongside lua/lazyverilog/checksums.lua.
export const RELEASE_CHECKSUMS: Record<string, Record<string, string>> = {
  "v1.3.4": {
    "linux-x64": "30b290c4eb050793ea2e2368741319eeff8fd5ca08feb515768d9a6fb4aa74a4",
    "linux-arm64": "1dca824c5a0691550c6ed49954307bf85acd59915eb021634eb24415d9dac8a6",
    "linux-x64-static": "01b81cd4f41ac4060a47b9e5282a3ef6519cbc86c34cabba059529af603eb5ff",
    "linux-arm64-static": "0de7cc8c31342cf221992544fdfe1aaec9a1c981375a289a4c6c4add3be03f0f",
    "darwin-x64": "50fbb988b65d0b6f1a5546f64649e44db0805e72eeded3cce6f127f0d96bf7c9",
    "darwin-arm64": "14fd36f5ac4997cafa1a3711079415895a66cee3883afbccf90234b97b10f09a",
    "windows-x64": "b7885be0362a24fb1b54b5402dc7d640456467106f1f6ce752c61c5562c2edc0",
  },
};
