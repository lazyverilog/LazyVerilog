// SHA-256 checksums keyed by version then platform.
// Updated by CI alongside lua/lazyverilog/checksums.lua.
export const RELEASE_CHECKSUMS: Record<string, Record<string, string>> = {
  "v2.0.1": {
    "linux-x64": "03eb0dbdc9d63ffa1b44e501b59b0ded25697e30589e1a23a6dea06160824acf",
    "linux-arm64": "61b5bde8e02cf66b6975eb20361c29b618e1bcfcd31d0f442d51c4e92f838aa3",
    "linux-x64-static": "90edbb9f2eab49d7c1144ff12c673ab4b464fe35bc9c5b121792e24c8dd95f52",
    "linux-arm64-static": "c5a0b4c3c43e20f9c007b28d611d16d9c1969e202ec4b2ad3667488e1c97f9e8",
    "darwin-x64": "5a90eb404847e1af559f3608cfe590a565e964c8246d8cab2fbb8230047acd72",
    "darwin-arm64": "0db2c8707522aeff842e2511ebd17ed6f7fcb64beda7f19d34c48b136735229d",
    "windows-x64": "b3f9fb25234492630198f2a265ef48ac6f8a639e41ec0cda1fcbad2436b3fb55",
  },
};
