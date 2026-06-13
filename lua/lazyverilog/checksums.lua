--- Release binary checksum lock file.
---
--- This file is intentionally committed to the plugin instead of downloaded at
--- install time.  The installer treats these SHA-256 values as the known-good
--- list for release assets and refuses to install a binary when the version or
--- platform is missing.
---
--- Release maintainers update this file before creating a release tag:
---
---   1. Build the exact release binaries.
---   2. Compute each binary's SHA-256 digest.
---   3. Add the digests under the release version below.
---   4. Tag the commit that contains both version.lua and these checksums.
---
--- Keep the shape simple because .github/workflows/release.yml validates the
--- built artifacts against this module before uploading them to a release.

return {
	["v1.2.8"] = {
		["linux-x64"] = "4f2b4184779e5de4ea9e1ab8bcdeab52f6eed39574cf7015e6bb25f142511a5b",
		["linux-arm64"] = "476017d148a47486af0e39db92c1b8b508683d4747b8e322d1d3b002b23f3355",
		["linux-x64-static"] = "54dc40bbee26850b8ff762fe3550d429847f820b3bef8fd17b7dd58c8e200e0a",
		["linux-arm64-static"] = "7456c1559c76826ccdcaab172bf6398dab9f9d3f010d01cd820b3fb4d7f7303e",
		["darwin-x64"] = "6b6a6f641ba723fa3505dc095cdae24aa336a988fb5ff36e512677a3874f3131",
		["darwin-arm64"] = "ec801b4bbdcc6fafcea77498db8e46a1d2db29a7a8c22d5806efd1cfb1a1ae75",
	},

	["v1.2.6"] = {
		["linux-x64"] = "ae1731fb4c64c4e8fc306990ae263e04715b314e69e6dbf61155c70085b72e6f",
		["linux-arm64"] = "99dc1d1cf848fc39a7c48b055ae3469d25cdf1210457524bc1d49bf6ac8a831c",
		["linux-x64-static"] = "03db81a70f3744d3947bff2574813c4ea227c11156760c39db7e332689b21aba",
		["linux-arm64-static"] = "a78418aebb74258cfe5ba4e9e6e84aca407cfe449043de96cc510b1995a45cbb",
		["darwin-x64"] = "139fa054771896a563624813e05cbdbe198042517e70023d768289a6197d2838",
		["darwin-arm64"] = "c69945cd9441180f97598cebaae82906668b4ac8173d544efa88a78fc9658e5e",
	},

	["v1.2.5"] = {
		["linux-x64"] = "77e3403cebb2b9b7addba417c19e9c356f42bd14dbef929ae35dd3f164caced1",
		["linux-arm64"] = "afdca092914e57e0527d3af1b256f824c5d7e4ad5b48e7c95dfc6c05c986367a",
		["linux-x64-static"] = "a14caeca22fbfc673d6c341be2c785fc2b2cdfc51ddb8c249ce14dbd216edaa0",
		["linux-arm64-static"] = "41f9456c0bd0743f71e917661ff5e42b0b5381a5f00fa15e366b3965b9b883f7",
		["darwin-x64"] = "a75a2b0455580b069a3c0adcc18888ed3e7cbe92239a708e98824a03a7a61763",
		["darwin-arm64"] = "923c85f70a1bda3ddfd1fe9b369a9dae13b869cab3814fa0cfee064f20336d86",
	},

	-- Add the next release as another ["vX.Y.Z"] table with the same platform
	-- keys used above.
}
