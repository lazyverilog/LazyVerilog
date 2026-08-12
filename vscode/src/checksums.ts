// SHA-256 checksums keyed by version then platform.
// Updated by CI alongside lua/lazyverilog/checksums.lua.
export const RELEASE_CHECKSUMS: Record<string, Record<string, string>> = {
  "v1.3.5": {
    "linux-x64": "110d79700c25aad0c2dd980ca7d60a186af240d61498d3cd17606bdeefe99bfa",
    "linux-arm64": "b683887de8178e3ea95389ba8961d2c535ce9ae62215d63da09950e9cc433dda",
    "linux-x64-static": "7c2b35b9662e13a7a33006583c4370f12045108f847168cc9ea31eb59371c3a6",
    "linux-arm64-static": "cf6815a51c6277814b91289199c273af509cb087f1edca229c54b8c1bbcc037d",
    "darwin-x64": "8084c0456b238c3d975cbc0e747fde900082aef3e7f84c1707b6a76a5c51e44d",
    "darwin-arm64": "1918ae3295da79fe5603f2d280522ee470c79c8d9398a8e5a4bedea123814f7f",
    "windows-x64": "a294c6cb4ba80e4f228e775bacd518558f190b85069506baf72f838608f39703",
  },
};
