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
	["v1.3.0"] = {
		["linux-x64"] = "210b399f0f29d9063420685d086cca80b173d6ad515d3a62cd76a773b82eeb2c",
		["linux-arm64"] = "4c61fdd8971f27487932a80cdaf7eaf34bbf9129a00b1d79493d924aa549cb4f",
		["linux-x64-static"] = "f327ac3a3f6d2d0ecbcc4376a0ae5cecad581ebe365089b1e7c0ab9dd790f2c8",
		["linux-arm64-static"] = "5dc52d2fa19c3238165a860065830d7bdf0dbc76acb2220871476ca6f858845d",
		["darwin-x64"] = "ee5db641d2f4a2f62312615126c31dac436a5b59d1933b4be21b082df8274cd0",
		["darwin-arm64"] = "ba82a9f7c92cd0ed7674d5cbce4f67012c06be45315cf2e2e0bb7c0942ccd742",
		["windows-x64"] = "ec497842fce77313537098f26e5cb780bc76d0fb70c636e888da73b279a6e5e4",
	},

	["v1.2.9"] = {
		["linux-x64"] = "19b51bda218ee9677cb5161c42054e8253256f11cf78f23ca48ede027a6b376b",
		["linux-arm64"] = "2aa4d0e8f9ab379a49a61b69c1d7fe953d90a165bd4d38ea9523f73978873122",
		["linux-x64-static"] = "9c0a80cf19a505f0f0b4b34b54d7cd1492022f3df1908fb85c415863616725f3",
		["linux-arm64-static"] = "33bc5530a6690799cdf7abb626f0853b69da549af2e5a01394021f8f62721c9f",
		["darwin-x64"] = "cff87d19c6f37486873023c6ec67b4b57c1e6eec794231ba15d2cadfb91a0f45",
		["darwin-arm64"] = "89bb7bb09bd752e75682d09a4cd3fc017ef2f076c8ed5b00d68ca5cee897119a",
		["windows-x64"] = "3c94ecbe9b6436a7062263adbded0676deda54103fe1942cb00ea2f6a86e1830",
	},

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
