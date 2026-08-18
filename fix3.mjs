import fs from "fs";
let s = fs.readFileSync("README.md", "utf8");
const anchor = "3. **Build** — open the project and let the editor compile it, or verify against both engines standalone:\n\n   ```powershell\n   .\scripts\build_all.ps1\n   ```\n";
if (!s.includes(anchor)) throw new Error("build step anchor not found");
s = s.replace(anchor, anchor + "\n   The link is a junction, so one clone serves every project you point it at — but the plugin's `Binaries/` and `Intermediate/` are shared along with the source. If you link the same clone into a 5.7 project *and* a 5.8 one, whichever you built last wins, and the other opens with \"modules are missing or built with a different engine version\". Delete `Plugin/Uplink/Binaries` and `Plugin/Uplink/Intermediate` and rebuild when you switch, or keep a separate clone per engine.\n");
fs.writeFileSync("README.md", s);
console.log("engine-switching note added");
