// SHA-256 checksums keyed by version then platform.
// Updated by CI alongside lua/lazyverilog/checksums.lua.
export const RELEASE_CHECKSUMS: Record<string, Record<string, string>> = {
  "v1.3.1": {
    "linux-x64": "5e30850e8dce625fc89cafe17d2ff2257e77398e28475d1d9343dd14d54004d7",
    "linux-arm64": "1408681a2ccbb79bd7caa195f384ec9c657ba279fdb1ef8532732f30cdd2a303",
    "linux-x64-static": "bb55ae37a04c74b183311cd2c0f768c9f125cc5d12b13b62d9da4fbc21456456",
    "linux-arm64-static": "50a4651c118ca38aeacdfa43cc690bcb15bd725d4c2659c4d661a677163ace92",
    "darwin-x64": "acb002a46807995ec460fdc61fe758d504fd5c0377538efe6dbac7016ae0d062",
    "darwin-arm64": "6b24258afe8e18f5ef2a2f23ad236a887dc3333e7ead69c8f51b90693ca08df2",
    "windows-x64": "b34e63c84f448d5f7a176ca218c1e9ba3afcfc2b09ab9136b2518c3cde0ce8de",
  },
};
