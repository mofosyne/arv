// Runs a WASI build of udfmake with Node's built-in WASI runtime (Node 20+):
//   node wasi/run.mjs build-wasi/udfmake -o T=bdrom,v=2.50,V=2.50 image.udf dir
// The whole host file system is visible to the program, as for a native build.
import { readFile } from 'node:fs/promises';
import { WASI } from 'node:wasi';
import { argv, env, exit } from 'node:process';

const wasi = new WASI({ version: 'preview1', args: argv.slice(2), env, preopens: { '/': '/' }, returnOnExit: true });
const module = await WebAssembly.compile(await readFile(argv[2]));
exit(wasi.start(await WebAssembly.instantiate(module, wasi.getImportObject())));
