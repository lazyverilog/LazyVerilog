// SHA-256 checksums keyed by version then platform.
// Updated by CI alongside lua/lazyverilog/checksums.lua.
export const RELEASE_CHECKSUMS: Record<string, Record<string, string>> = {
  "v2.0.0": {
    "linux-x64": "f857f41ef8211a5037570eedb3b244334b8d4ff306b60425e7d0f1efbc435fbe",
    "linux-arm64": "c25df999b3c096b63548c6db8fec8bd650e12cc737e6f69854a7fa1ec6649925",
    "linux-x64-static": "3531cb75e9020dca0668bc2ccd5784b819bd90731ef178063fa36c80eb3c29bd",
    "linux-arm64-static": "05cdd12da409e5a552fd82eb8d1271a347dea01c850f60e6c0e74aaca7162074",
    "darwin-x64": "7b1ed72bd6d3590a4f83c9e11fe9e200c7cecbbaee77124cba3ae9436a31af7c",
    "darwin-arm64": "dd59c007e0860824768e70434c15d581309c21303adee2a8c7f0a6a12a45f8ec",
    "windows-x64": "83cb638ac4c438059c0802e9a1af480ebdf1c93d7d5a242bbde4ad673ad2c061",
  },
};
