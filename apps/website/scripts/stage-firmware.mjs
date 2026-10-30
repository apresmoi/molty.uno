// Stage firmware for the /setup page (ESP Web Tools): one ESP Web Tools manifest per
// board under public/firmware/<board-name>/, plus public/firmware/index.json listing the
// boards. The images are served from the site itself (same origin): GitHub release
// assets have no CORS headers, so the browser could not fetch them from there.
//
// The board list comes from device/firmware/boards/**/board.json (folders starting with "_"
// are not boards), like tools/scripts/firmware-boards.py. The output folder is gitignored.
//
//   node scripts/stage-firmware.mjs --local [build-root]   images from local builds:
//        <build-root>/<board-name>/ (default ../../device/firmware/build; `make firmware BOARD=...`),
//        flashed part by part at the offsets in each build's flash_args
//   node scripts/stage-firmware.mjs --release [tag]        molty-<ver>-<board>-factory.bin
//        from a GitHub release (default: the latest), via the `gh` CLI
//   node scripts/stage-firmware.mjs --none                 manifests only, no images
//
// A board without an image is still listed (available: false) so the page can say so;
// a missing release is a warning, not an error, so the site still deploys.
import { readFileSync, writeFileSync, mkdirSync, rmSync, existsSync, readdirSync, statSync, copyFileSync } from 'node:fs';
import { join, basename } from 'node:path';
import { fileURLToPath } from 'node:url';
import { execFileSync } from 'node:child_process';

const ROOT = fileURLToPath(new URL('../../../', import.meta.url));
const OUT = fileURLToPath(new URL('../public/firmware/', import.meta.url));
const BOARDS = join(ROOT, 'device/firmware/boards');

const [mode = '--none', arg] = process.argv.slice(2);
if (!['--local', '--release', '--none'].includes(mode)) {
  console.error('usage: stage-firmware.mjs --local [build-root] | --release [tag] | --none');
  process.exit(2);
}

/** device/firmware/boards/<vendor>/<model>/board.json, skipping "_" folders */
function boards() {
  const out = [];
  const walk = (dir, rel) => {
    for (const e of readdirSync(dir, { withFileTypes: true })) {
      if (!e.isDirectory() || e.name.startsWith('_')) continue;
      const sub = join(dir, e.name);
      if (existsSync(join(sub, 'board.json'))) out.push({ dir: [...rel, e.name].join('/'), ...JSON.parse(readFileSync(join(sub, 'board.json'), 'utf8')) });
      else walk(sub, [...rel, e.name]);
    }
  };
  walk(BOARDS, []);
  return out.sort((a, b) => a.name.localeCompare(b.name));
}

/** ESP Web Tools manifest. name must equal the firmware name the device reports over
 *  Improv ("molty.UNO", net/improv_serial.c) so a connected Molty reads as "same firmware". */
const manifest = (version, parts) => ({
  name: 'molty.UNO',
  version,
  new_install_prompt_erase: true,
  // after a fresh install the board boots, mounts storage, then answers Improv
  new_install_improv_wait_time: 20,
  builds: parts ? [{ chipFamily: 'ESP32-S3', parts }] : [],
});

/** "--flash_mode dio ...\n0x0 bootloader/bootloader.bin\n..." → [{ offset, file }] */
function flashArgs(path) {
  return readFileSync(path, 'utf8')
    .split('\n')
    .map((l) => l.trim().split(/\s+/))
    .filter(([off, file]) => /^0x[0-9a-f]+$/i.test(off ?? '') && file)
    .map(([off, file]) => ({ offset: parseInt(off, 16), file }));
}

function gh(args) {
  return execFileSync('gh', args, { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
}

function repoSlug() {
  if (process.env.GITHUB_REPOSITORY) return process.env.GITHUB_REPOSITORY;
  const url = execFileSync('git', ['-C', ROOT, 'remote', 'get-url', 'origin'], { encoding: 'utf8' }).trim();
  const m = /github\.com[:/]([^/]+\/[^/]+?)(\.git)?$/.exec(url);
  if (!m) throw new Error(`cannot tell the GitHub repo from origin ${url}`);
  return m[1];
}

rmSync(OUT, { recursive: true, force: true });
mkdirSync(OUT, { recursive: true });
const version = readFileSync(join(ROOT, 'VERSION'), 'utf8').trim();
const index = { source: mode.slice(2), version: null, boards: [] };

let release = null;
if (mode === '--release') {
  try {
    const repo = repoSlug();
    const view = JSON.parse(gh(['release', 'view', ...(arg ? [arg] : []), '--repo', repo, '--json', 'tagName,assets']));
    release = { repo, tag: view.tagName, version: view.tagName.replace(/^v/, ''), assets: view.assets.map((a) => a.name) };
    index.version = release.version;
    console.log(`stage-firmware: release ${release.tag} of ${repo}`);
  } catch (err) {
    console.warn(`stage-firmware: no release to stage (${String(err.message ?? err).split('\n')[0]}); manifests only`);
  }
}

for (const b of boards()) {
  const dest = join(OUT, b.name);
  mkdirSync(dest, { recursive: true });
  let parts = null;
  let ver = version;

  if (mode === '--local') {
    const build = join(arg ?? join(ROOT, 'device/firmware/build'), b.name);
    if (existsSync(join(build, 'flash_args')) && existsSync(join(build, 'molty.bin'))) {
      parts = flashArgs(join(build, 'flash_args')).map(({ offset, file }) => {
        const name = basename(file);
        copyFileSync(join(build, file), join(dest, name));
        return { path: name, offset };
      });
      ver = `${version}-local`;
      index.version = ver;
      console.log(`stage-firmware: ${b.name}: ${parts.length} parts from ${build}`);
    } else console.warn(`stage-firmware: ${b.name}: no build in ${build} (make firmware BOARD=${b.dir})`);
  }

  if (release) {
    const asset = `molty-${release.version}-${b.name}-factory.bin`;
    if (release.assets.includes(asset)) {
      gh(['release', 'download', release.tag, '--repo', release.repo, '--pattern', asset, '--dir', dest, '--clobber']);
      parts = [{ path: asset, offset: 0 }];
      ver = release.version;
      console.log(`stage-firmware: ${b.name}: ${asset} (${(statSync(join(dest, asset)).size / 1048576).toFixed(1)} MB)`);
    } else console.warn(`stage-firmware: ${b.name}: ${release.tag} has no ${asset}`);
  }

  writeFileSync(join(dest, 'manifest.json'), JSON.stringify(manifest(ver, parts), null, 2) + '\n');
  index.boards.push({
    name: b.name,
    title: b.title,
    status: b.status,
    display: b.display,
    caps: b.caps,
    serial: b.caps.includes('serial'),
    // identification (src/features/setup/match.ts): the USB IDs the board shows up as, and
    // its download-mode fingerprint
    usb: b.usb.map((u) => ({ vid: parseInt(u.vid, 16), pid: parseInt(u.pid, 16) })),
    detect: b.detect,
    manifest: `/firmware/${b.name}/manifest.json`,
    available: parts !== null,
  });
}

writeFileSync(join(OUT, 'index.json'), JSON.stringify(index, null, 2) + '\n');
console.log(`stage-firmware: ${index.boards.filter((b) => b.available).length}/${index.boards.length} boards with firmware -> public/firmware/`);
