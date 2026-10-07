// BlockBreach (ImmortalSouull, MIT): Minecraft Java runs next to Ready or Not and is drawn into its picture. A UE4SS C++
// mod (RoNPassthrough, main.dll) + ReShade add-on in one DLL on the Ready or Not side, linked over
// ws://127.0.0.1:25599 to a Fabric mod (rehan's mc-gta5-passthrough example, adapted). Rehosted on SIGFAI/blockbreach
// (standard upstream fusion, source.hosted).
//
// Upstream ships an installer (BlockBreach.exe, which we do not use) and BlockBreach-1.0.0-manual.zip, the same payload
// laid out as it installs. Ours is that payload, files unchanged:
//   - blockbreach-ron.zip -> {game}/ReadyOrNot/Binaries/Win64: dwmapi.dll (UE4SS proxy) + ue4ss/ (UE4SS.dll, its
//     settings, mods.txt, Mods/RoNPassthrough/dlls/main.dll), dxgi.dll (= the official ReShade64.dll 6.8.0, byte for
//     byte), ReShade.ini/ReShadePreset.ini, reshade-shaders/, plus upstream's LICENSE and THIRD_PARTY_NOTICES.md
//     under blockbreach-licenses/.
//   - blockbreach.mrpack: Minecraft 26.3 + Fabric 0.19.5, upstream's blockbreach-passthrough.jar and options.txt,
//     Fabric API 0.161.0+26.3 as a Modrinth link (upstream's copy is the same file).
// UE4SS.dll is the author's own build of RE-UE4SS (main.dll links against it); its export table is identical to the
// official experimental-latest v3.0.1-1161-g6eb3d9bc, see notes.md.
//   node library/blockbreach/build.mjs       (outputs: library/lib.mjs)
import { mrpack, resolveFabricApi, sha256, unzip } from '../../orchestrator/src/recipe.js';
import { asset, card, dl, emit, pinned, player, zipAsset } from '../lib.mjs';

const UP = {
  repo: 'https://github.com/ImmortalSouull/BlockBreach', tag: 'v1.0.0', commit: 'ffe5ec8384ce51b9030fa2edeca0281f60f45bac',
  license: 'MIT', authors: ['ImmortalSouull'],
  zip: { file: 'BlockBreach-1.0.0-manual.zip', sha256: '9b40c89f33e514a8f6f6ba32fadbfe05c31e12f900e0e20ca64b17644acb5dc1' }, // = GitHub digest, 2026-10-07
};
const ID = 'blockbreach', VERSION = '1.0.0', NAME = 'BlockBreach';
const MC = { mc: '26.3', loader: '0.19.5', fabricApi: '0.161.0+26.3', java: '25' }; // minecraft-mod/gradle.properties
const TAGLINE = 'Minecraft inside Ready or Not: build cover mid-raid, set zombies on the suspects, blow walls with TNT.';
const W64 = 'ReadyOrNot/Binaries/Win64/';
// Reviewed hashes (notes.md): a different release file fails the build.
const PINNED = {
  [`${W64}dwmapi.dll`]: 'cf440b9eb8643bb7c434acfda696aee57fd981d185dca5e57fb8dbb18f8fc1cd',
  [`${W64}dxgi.dll`]: '0cee63f9c9f13f3ac909c5b4903f4dbb4b719a7ab3b4f13b0deaf83c814b94f7', // official ReShade64.dll 6.8.0
  [`${W64}ue4ss/UE4SS.dll`]: 'ec7fc753303d9d76a3c090557614abb1a74551bcf5b1e1267c3623d65539f251',
  [`${W64}ue4ss/Mods/RoNPassthrough/dlls/main.dll`]: 'ad658064aa150f68641172d62f970087a644dee890380925db50573f72a75e0e',
  'Minecraft/mods/blockbreach-passthrough.jar': 'c4169886b758cd821929549ce8d8ae6d1bdafd615de79617385af5cee1cf1e34',
};
const JAR = 'blockbreach-passthrough.jar';

const up = new Map(unzip(await pinned(`${UP.repo}/releases/download/${UP.tag}/${UP.zip.file}`, UP.zip.sha256)).map(e => [e.name.replace(/\\/g, '/'), e.data]));
for (const [f, h] of Object.entries(PINNED)) if (!up.has(f) || sha256(up.get(f)) !== h) throw new Error(`${UP.zip.file}: ${f} missing or not the reviewed build`);

const ron = zipAsset(`${ID}-ron.zip`, [
  ...[...up.keys()].filter(f => f.startsWith(W64)).map(f => ({ name: f.slice(W64.length), data: up.get(f) })),
  { name: 'blockbreach-licenses/LICENSE', data: up.get('LICENSE') },
  { name: 'blockbreach-licenses/THIRD_PARTY_NOTICES.md', data: up.get('THIRD_PARTY_NOTICES.md') },
]);
const fabricApi = await resolveFabricApi(MC.fabricApi, MC.mc);
if (!fabricApi?.download) throw new Error(`Fabric API ${MC.fabricApi} not resolved on Modrinth`);
const pack = asset(`${ID}.mrpack`, mrpack({ name: NAME, summary: TAGLINE, versions: MC, versionId: VERSION, fabricApi,
  jars: [{ name: JAR, data: up.get(`Minecraft/mods/${JAR}`) }],
  extra: [
    { name: 'overrides/options.txt', data: up.get('Minecraft/options.txt') },
    { name: 'overrides/licenses/blockbreach-LICENSE.txt', data: up.get('LICENSE') },
  ] }));
const assets = [ron, pack];

const make = (urls, set) => {
  const mp = set.find(a => a.name.endsWith('.mrpack'));
  return {
    id: `sigf/${ID}`,
    version: VERSION,
    name: NAME,
    tagline: player(ID).tagline ?? TAGLINE,
    how_to_play: player(ID).howToPlay,
    kind: 'passthrough',
    games: [
      { game: 'readyornot', role: 'host', label: 'Ready or Not', engine: 'Ready or Not (Unreal Engine 5, DirectX 11) + UE4SS C++ mod RoNPassthrough + ReShade 6.8.0 add-on',
        apps: { steam: '1144200' }, runtime: 'tested by the author on Ready or Not build 133804 (game build number; no Steam build id given), DirectX 11 only' },
      { game: 'minecraft', role: 'guest', label: 'Minecraft', engine: 'Minecraft Java 26.3 + Fabric mod passthrough (Java)', mc: MC.mc, loader: `fabric@${MC.loader}`, java: MC.java },
    ],
    requires: [
      { id: 'ue4ss', version: '3.0.1-1161-g6eb3d9bc (author build)', license: 'MIT, shipped unchanged', page: 'https://github.com/UE4SS-RE/RE-UE4SS', note: 'in the mod files, installed into Ready or Not by the app' },
      { id: 'reshade', version: '6.8.0', license: 'BSD-3-Clause, shipped unchanged (as dxgi.dll)', page: 'https://reshade.me/', note: 'in the mod files, installed into Ready or Not by the app' },
      { id: 'fabric-loader', version: MC.loader },
      { id: 'fabric-api', version: MC.fabricApi, note: 'in the Minecraft pack (downloaded from Modrinth)' },
    ],
    install: [
      { game: 'readyornot', strategy: 'game-dir-snapshot', loader: 'ue4ss', files: [
        { src: ron.name, dst: `{game}/${W64.slice(0, -1)}`, unpack: true, contents: ron.contents, ...dl(ron, urls) },
      ] },
      { game: 'minecraft', strategy: 'mrpack', pack: { src: mp.name, ...dl(mp, urls) } },
    ],
    // Minecraft first: its mod opens its own void world and serves the link on 127.0.0.1:25599. Ready or Not on DX11
    // (-dx11, what upstream's launcher passes): the compositor only works on DirectX 11.
    launch: [{ game: 'minecraft', wait: 'port:25599' }, { game: 'readyornot', args: ['-dx11'] }],
    files: set.map(a => ({ name: a.name, ...dl(a, urls) })),
    source: {
      repo: UP.repo, license: 'MIT AND BSD-3-Clause', upstream_license: UP.license, tag: UP.tag, commit: UP.commit,
      hosted: `https://github.com/SIGFAI/${ID}`,
      based_on: 'https://github.com/rehan-remade/universal-modder',
      bundled: [
        { name: 'UE4SS (author build of RE-UE4SS)', version: '3.0.1-1161-g6eb3d9bc', repo: 'https://github.com/UE4SS-RE/RE-UE4SS', license: 'MIT' },
        { name: 'ReShade', version: '6.8.0', repo: 'https://github.com/crosire/reshade', commit: '18deaa52de0c425a78b329e9cb3c497281cd00ec', license: 'BSD-3-Clause' },
      ],
    },
    media: {},
    built_by: { author: UP.authors[0], authors: [...UP.authors, 'Rehan and universal-modder contributors'], packaged_by: 'SIGF' },
    idea_by: UP.authors[0],
    built_at: '2026-10-07T00:00:00.000Z',
    // Never installed together (the app refuses either order): ReadyCraft: both load their own proxy DLLs from Ready or Not's Binaries/Win64 and hook the same game.
    conflicts: ['sigf/readycraft'],
    ...card(UP.repo),
    notes: player(ID).notes,
  };
};

emit({ slug: ID, version: VERSION, assets, make });
