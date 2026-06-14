// SHA-256 checksums keyed by version then platform.
// Updated by CI alongside lua/lazyverilog/checksums.lua.
export const RELEASE_CHECKSUMS: Record<string, Record<string, string>> = {
  "v1.3.0": {
    "linux-x64": "210b399f0f29d9063420685d086cca80b173d6ad515d3a62cd76a773b82eeb2c",
    "linux-arm64": "4c61fdd8971f27487932a80cdaf7eaf34bbf9129a00b1d79493d924aa549cb4f",
    "linux-x64-static": "f327ac3a3f6d2d0ecbcc4376a0ae5cecad581ebe365089b1e7c0ab9dd790f2c8",
    "linux-arm64-static": "5dc52d2fa19c3238165a860065830d7bdf0dbc76acb2220871476ca6f858845d",
    "darwin-x64": "ee5db641d2f4a2f62312615126c31dac436a5b59d1933b4be21b082df8274cd0",
    "darwin-arm64": "ba82a9f7c92cd0ed7674d5cbce4f67012c06be45315cf2e2e0bb7c0942ccd742",
    "windows-x64": "ec497842fce77313537098f26e5cb780bc76d0fb70c636e888da73b279a6e5e4",
  },
};
