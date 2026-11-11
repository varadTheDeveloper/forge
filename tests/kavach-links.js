// Phase 8.3: links inside a folder granted for reading and writing.
//   Forge.args[0]: a hard link to a file outside the grant
//   Forge.args[1]: a junction to a folder outside the grant
//   Forge.args[2]: an ordinary file path inside the grant
// Writing the hard link must be refused (it would change the outside
// file), and removing the junction must remove only the link.
const [hardLink, junction, plain] = Forge.args;

try {
  fs.writeFileSync(hardLink, "overwritten");
  console.log("SHOULD NOT WRITE the hard link");
} catch (e) {
  console.log("hardlink: " + e.code + (e.message.includes("hard link") ? " explained" : " " + e.message));
}

fs.rmSync(junction, { recursive: true });
console.log("junction removed: " + !fs.existsSync(junction));

fs.writeFileSync(plain, "plain-ok");
console.log("plain: " + fs.readFileSync(plain, "utf8"));
