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
	["v1.3.6"] = {
		["linux-x64"] = "e0129ad0c52d5f95db40fb9f9d94744487a0b81b0033c7b4f56a280cbfff1652",
		["linux-arm64"] = "e801e526a7886369f87f05c9a155927652b3693377e89d387bc58a251679d1ea",
		["linux-x64-static"] = "5b7a25b73ac36eeda0d5e29f2a199eedd0aefe4a3b645407c34ec3eb1f0d35b0",
		["linux-arm64-static"] = "119c3131741922f4487e8c064dbd3bfe95a79465de805ff5cf50c87c1a9f16d7",
		["darwin-x64"] = "de4b7a6cbd4899a2aa991c082624527d8663b7359b6fc430a5cb62ec86404994",
		["darwin-arm64"] = "6ec5122aa2c9b2804ef3f355b277a4f7400598386f3fda8c55cfa7a7b4dba3c6",
		["windows-x64"] = "69fb2108cf9920a0e679a0dcad856aece870ff55cd193c8b77b6c5571aff161a",
	},

	["v1.3.5"] = {
		["linux-x64"] = "110d79700c25aad0c2dd980ca7d60a186af240d61498d3cd17606bdeefe99bfa",
		["linux-arm64"] = "b683887de8178e3ea95389ba8961d2c535ce9ae62215d63da09950e9cc433dda",
		["linux-x64-static"] = "7c2b35b9662e13a7a33006583c4370f12045108f847168cc9ea31eb59371c3a6",
		["linux-arm64-static"] = "cf6815a51c6277814b91289199c273af509cb087f1edca229c54b8c1bbcc037d",
		["darwin-x64"] = "8084c0456b238c3d975cbc0e747fde900082aef3e7f84c1707b6a76a5c51e44d",
		["darwin-arm64"] = "1918ae3295da79fe5603f2d280522ee470c79c8d9398a8e5a4bedea123814f7f",
		["windows-x64"] = "a294c6cb4ba80e4f228e775bacd518558f190b85069506baf72f838608f39703",
	},

	["v1.3.4"] = {
		["linux-x64"] = "30b290c4eb050793ea2e2368741319eeff8fd5ca08feb515768d9a6fb4aa74a4",
		["linux-arm64"] = "1dca824c5a0691550c6ed49954307bf85acd59915eb021634eb24415d9dac8a6",
		["linux-x64-static"] = "01b81cd4f41ac4060a47b9e5282a3ef6519cbc86c34cabba059529af603eb5ff",
		["linux-arm64-static"] = "0de7cc8c31342cf221992544fdfe1aaec9a1c981375a289a4c6c4add3be03f0f",
		["darwin-x64"] = "50fbb988b65d0b6f1a5546f64649e44db0805e72eeded3cce6f127f0d96bf7c9",
		["darwin-arm64"] = "14fd36f5ac4997cafa1a3711079415895a66cee3883afbccf90234b97b10f09a",
		["windows-x64"] = "b7885be0362a24fb1b54b5402dc7d640456467106f1f6ce752c61c5562c2edc0",
	},

	["v1.3.3"] = {
		["linux-x64"] = "59aa0060ea92a89a7f3e072e037dc3d2ef8d62ca08ac2f38c5d0bd7b1f77bf1c",
		["linux-arm64"] = "584fd07bae69088bb23ada8453bfed31bdc515402996dad0dbc9bf85902c72c4",
		["linux-x64-static"] = "21c67369ccc77a1ad7427c14bc4cdb2076733f5901b17b77f7e417ac08eeb5ab",
		["linux-arm64-static"] = "d344025442b7f708fce95108a1c523e3fa6d72d7bdb900d750dce40254499d95",
		["darwin-x64"] = "63e04afb3bf4d0b19510f3571194b51048ac83f1c0fddca350cf23d768972040",
		["darwin-arm64"] = "b4327379795cfd57f8fd953fdfba0457988dced712cbd19c7ed278a606b1ac7d",
		["windows-x64"] = "c29b3337f4d5c13ae9f2d2199f6ccc006472b51250159a2d51fd6c099e6fc44c",
	},

	["v1.3.2"] = {
		["linux-x64"] = "b0a62f6a81c4304eed526529ec6e2d8191a7d2a88ca49cc7fe5e3b45bb4fe430",
		["linux-arm64"] = "ba6352166b072e11c569f3b866629e59c00a1914dbff238c4ebe0a78471ea7f2",
		["linux-x64-static"] = "1328f9282effcd6af3ac7f6e5cb2227b0369036e4490e015cd5d808244f39109",
		["linux-arm64-static"] = "281ab000359b8fd41ee9cd67e21a76e3f2e576ed145226f830361fa9b72036f8",
		["darwin-x64"] = "8ea3887ac0d2826e70e7ac325f69d19b4b59b31a2e7895ce2006ec316abee26e",
		["darwin-arm64"] = "2bd744ad14d61b8baa881908504cc1b6c425cc1aa5201b273ec9fe20e357e905",
		["windows-x64"] = "61523b1c73d8098cd6f2f53cc1e6d28abb2abc4605bbc9b4a83761f0b5a128ad",
	},

	["v1.3.1"] = {
		["linux-x64"] = "5e30850e8dce625fc89cafe17d2ff2257e77398e28475d1d9343dd14d54004d7",
		["linux-arm64"] = "1408681a2ccbb79bd7caa195f384ec9c657ba279fdb1ef8532732f30cdd2a303",
		["linux-x64-static"] = "bb55ae37a04c74b183311cd2c0f768c9f125cc5d12b13b62d9da4fbc21456456",
		["linux-arm64-static"] = "50a4651c118ca38aeacdfa43cc690bcb15bd725d4c2659c4d661a677163ace92",
		["darwin-x64"] = "acb002a46807995ec460fdc61fe758d504fd5c0377538efe6dbac7016ae0d062",
		["darwin-arm64"] = "6b24258afe8e18f5ef2a2f23ad236a887dc3333e7ead69c8f51b90693ca08df2",
		["windows-x64"] = "b34e63c84f448d5f7a176ca218c1e9ba3afcfc2b09ab9136b2518c3cde0ce8de",
	},

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
