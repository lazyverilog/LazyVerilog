// SHA-256 checksums keyed by version then platform.
// Updated by CI alongside lua/lazyverilog/checksums.lua.
export const RELEASE_CHECKSUMS: Record<string, Record<string, string>> = {
  "v2.1.2": {
    "linux-x64": "1080578ba542dfba19ea9574e0bcd14611d7e04f4d1bb4ef820bca93a7f581d3",
    "linux-arm64": "aec446fcb233ffc8ea9e26622fab461b239e0c6c335e9eda355cd92a1e7414c3",
    "linux-x64-static": "a6ef081ea1577e8f8aaea527d2742ab81713cae9bd96e1dd6d74a7a6ee9cd564",
    "linux-arm64-static": "f6f6bb508f610141781760ae5a334eb671be6cec3d8a474c63c41b5304b1cd62",
    "darwin-x64": "32e08f44f509b4e1df39659bdd92b09ba430291c33cfaa5f6d774c601cf5c9d1",
    "darwin-arm64": "69cc480add3f5031ec088bce6734d2993757fee96a9fcb4e0466bb5fde0e25f2",
    "windows-x64": "f7c56758d788487d960ca698c22357be4cedf63931ee9da75d7a6b8e8b2c557e",
  },
};
