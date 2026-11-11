// Phase 8.3: the Promise-returning fs functions, in a granted folder
// (Forge.args[0]), and an async refusal (Forge.args[1], outside every
// grant), which must be a rejected Promise.
const base = Forge.args[0];

(async () => {
  await fs.mkdir(base + "/d/e", { recursive: true });
  await fs.writeFile(base + "/d/f.txt", "abc");
  await fs.copyFile(base + "/d/f.txt", base + "/d/g.txt");
  await fs.rename(base + "/d/g.txt", base + "/d/e/h.txt");
  const names = (await fs.readdir(base + "/d")).sort().join();
  const typed = (await fs.readdir(base + "/d", { withFileTypes: true }))
    .map((e) => e.name + ":" + e.type).sort().join();
  const st = await fs.stat(base + "/d/e/h.txt");
  const lst = await fs.lstat(base + "/d");
  const real = await fs.realpath(base + "/d/e/../f.txt");
  console.log("names: " + names);
  console.log("typed: " + typed);
  console.log("stat: " + st.size + " " + st.type);
  console.log("lstat: " + lst.type);
  console.log("realpath ends: " + real.endsWith("f.txt"));
  await fs.rm(base + "/d", { recursive: true });
  console.log("gone: " + !(await fs.exists(base + "/d")));
  try {
    await fs.readFile(Forge.args[1]);
    console.log("SHOULD NOT READ");
  } catch (e) {
    console.log("async denied: " + e.code);
  }
})();
