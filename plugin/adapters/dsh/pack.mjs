// Packs this plugin into the repo's dist/ as miqipet-dsh-<version>.tgz. Users install it by pasting
// its GitHub raw URL into DeepSeek Harness's plugin manager, which hands it to pnpm. pnpm treats new
// bytes at a URL it already knows as tampering: `pnpm update` refuses, and removing and adding it again
// fails on the lockfile. So a published version is never overwritten, and a change needs a new version.
import { execFileSync } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const dist = join(here, "..", "..", "..", "dist");
const { name, version } = JSON.parse(readFileSync(join(here, "package.json"), "utf8"));
const file = `${name}-${version}.tgz`;
const target = join(dist, file);

const scratch = mkdtempSync(join(tmpdir(), "miqipet-dsh-pack-"));
try {
  execFileSync("npm", ["pack", "--pack-destination", scratch], {
    cwd: here, stdio: "inherit", shell: process.platform === "win32",
  });
  const packed = readFileSync(join(scratch, file));
  if (existsSync(target)) {
    if (readFileSync(target).equals(packed)) {
      console.log(`dist/${file} is already up to date`);
      process.exit(0);
    }
    console.error(`dist/${file} is already published with different content.`
      + ` Bump "version" in plugin/adapters/dsh/package.json, then pack again.`);
    process.exit(1);
  }
  mkdirSync(dist, { recursive: true });
  copyFileSync(join(scratch, file), target);
  console.log(`wrote dist/${file}`);
} finally {
  rmSync(scratch, { recursive: true, force: true });
}
