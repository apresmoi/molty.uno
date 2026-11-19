// The firmware's renderer as the site ships it (device/firmware/web/built/molty.wasm), fetched
// and compiled once, when the first thing that needs it asks: never with the page itself.
import wasmUrl from "@molty-wasm/molty.wasm?url";

let load: Promise<WebAssembly.Module> | null = null;
export function loadModule(): Promise<WebAssembly.Module> {
  return (load ??= WebAssembly.compileStreaming(fetch(wasmUrl)).catch(async () =>
    // a server that does not say application/wasm
    WebAssembly.compile(await (await fetch(wasmUrl)).arrayBuffer()),
  ));
}
