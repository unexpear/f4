import { createHash } from "node:crypto";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import { basename, dirname, extname, join, normalize, resolve, sep } from "node:path";
import { spawn, spawnSync } from "node:child_process";
import { inflateSync } from "node:zlib";

// Locked M11 64x64 RGBA8 fixture. OpenGL and DX12 produce these bytes exactly;
// Vulkan differs by at most one channel level on the reference test machine.
const m11ReferenceDeflateBase64 = readFileSync(
    new URL("../tests/fixtures/m11_reference.deflate.base64", import.meta.url),
    "utf8",
).trim();

const delay = (milliseconds) => new Promise((resolveDelay) => setTimeout(resolveDelay, milliseconds));

function findBrowser() {
    const candidates = process.env.F4_WEBGPU_BROWSER
        ? [process.env.F4_WEBGPU_BROWSER]
        : process.platform === "win32"
          ? [
                "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
                "C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
                "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
            ]
          : process.platform === "darwin"
            ? [
                  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
                  "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
              ]
            : ["/usr/bin/google-chrome", "/usr/bin/chromium", "/usr/bin/chromium-browser"];

    const browser = candidates.find((candidate) => existsSync(candidate));
    if (!browser) {
        throw new Error("No Chromium browser found; set F4_WEBGPU_BROWSER to its executable");
    }
    return browser;
}

function contentType(path) {
    return {
        ".html": "text/html; charset=utf-8",
        ".js": "text/javascript; charset=utf-8",
        ".wasm": "application/wasm",
    }[extname(path)] ?? "application/octet-stream";
}

async function startServer(root) {
    const normalizedRoot = `${resolve(root)}${sep}`;
    const server = createServer((request, response) => {
        const requestPath = decodeURIComponent(new URL(request.url, "http://localhost").pathname);
        const filePath = normalize(join(normalizedRoot, requestPath));
        if (!filePath.startsWith(normalizedRoot) || !existsSync(filePath)) {
            response.writeHead(404).end();
            return;
        }
        response.writeHead(200, { "Content-Type": contentType(filePath), "Cache-Control": "no-store" });
        response.end(readFileSync(filePath));
    });
    await new Promise((resolveListen, rejectListen) => {
        server.once("error", rejectListen);
        server.listen(0, "127.0.0.1", resolveListen);
    });
    return server;
}

async function waitForFile(path, timeoutMilliseconds) {
    const deadline = Date.now() + timeoutMilliseconds;
    while (Date.now() < deadline) {
        if (existsSync(path)) return;
        await delay(50);
    }
    throw new Error(`Timed out waiting for ${path}`);
}

async function removeProfile(path) {
    for (let attempt = 0; attempt < 20; ++attempt) {
        try {
            rmSync(path, { recursive: true, force: true });
            return;
        } catch (error) {
            if (attempt === 19) {
                console.warn(`Could not remove temporary browser profile ${path}: ${error.message}`);
                return;
            }
            await delay(100);
        }
    }
}

async function fetchJson(url, timeoutMilliseconds = 5000) {
    const response = await fetch(url, { signal: AbortSignal.timeout(timeoutMilliseconds) });
    if (!response.ok) throw new Error(`${url} returned HTTP ${response.status}`);
    return response.json();
}

class DevTools {
    static async connect(url) {
        const socket = new WebSocket(url);
        await new Promise((resolveOpen, rejectOpen) => {
            socket.addEventListener("open", resolveOpen, { once: true });
            socket.addEventListener("error", rejectOpen, { once: true });
        });
        return new DevTools(socket);
    }

    constructor(socket) {
        this.socket = socket;
        this.nextId = 1;
        this.pending = new Map();
        this.events = [];
        socket.addEventListener("message", (event) => {
            const message = JSON.parse(event.data);
            if (message.id !== undefined && this.pending.has(message.id)) {
                const { resolveCall, rejectCall } = this.pending.get(message.id);
                this.pending.delete(message.id);
                if (message.error) rejectCall(new Error(message.error.message));
                else resolveCall(message.result);
                return;
            }
            this.events.push(message);
        });
    }

    call(method, params = {}) {
        const id = this.nextId++;
        const response = new Promise((resolveCall, rejectCall) => {
            this.pending.set(id, { resolveCall, rejectCall });
        });
        this.socket.send(JSON.stringify({ id, method, params }));
        return response;
    }

    close() {
        this.socket.close();
    }
}

async function waitForTarget(port, timeoutMilliseconds) {
    const deadline = Date.now() + timeoutMilliseconds;
    while (Date.now() < deadline) {
        try {
            const targets = await fetchJson(`http://127.0.0.1:${port}/json/list`);
            const target = targets.find((candidate) => candidate.type === "page" && candidate.url === "about:blank");
            if (target) return target;
        } catch {
            // DevTools can accept TCP connections before its target list is ready.
        }
        await delay(50);
    }
    throw new Error("Timed out waiting for a blank Chromium page target");
}

async function evaluateJson(devTools, expression) {
    const result = await devTools.call("Runtime.evaluate", {
        expression: `JSON.stringify(${expression})`,
        returnByValue: true,
    });
    if (result.exceptionDetails) throw new Error(result.exceptionDetails.text);
    return JSON.parse(result.result.value);
}

async function evaluateJsonAsync(devTools, expression) {
    const result = await devTools.call("Runtime.evaluate", {
        expression: `(async () => JSON.stringify(await (${expression})))()`,
        awaitPromise: true,
        returnByValue: true,
    });
    if (result.exceptionDetails) throw new Error(result.exceptionDetails.text);
    return JSON.parse(result.result.value);
}

function browserErrors(events) {
    const errors = [];
    for (const event of events) {
        if (event.method === "Runtime.exceptionThrown") {
            const details = event.params.exceptionDetails;
            errors.push(details.exception?.description ?? details.text);
        }
        if (event.method === "Runtime.consoleAPICalled" && event.params.type === "error") {
            errors.push(event.params.args.map((argument) => argument.value ?? argument.description).join(" "));
        }
        if (event.method === "Log.entryAdded" && event.params.entry.level === "error") {
            errors.push(event.params.entry.text);
        }
    }
    return [...new Set(errors.filter(Boolean))];
}

async function run() {
    const htmlPath = resolve(process.argv[2] ?? "");
    const captureDirectory = resolve(process.argv[3] ?? join(dirname(htmlPath), "webgpu-captures"));
    if (!existsSync(htmlPath) || extname(htmlPath) !== ".html") {
        throw new Error(`Expected a built WebGPU HTML file, received ${htmlPath}`);
    }

    const browser = findBrowser();
    const server = await startServer(dirname(htmlPath));
    const address = server.address();
    const url = `http://127.0.0.1:${address.port}/${encodeURIComponent(basename(htmlPath))}`;
    const profile = mkdtempSync(join(tmpdir(), "f4-webgpu-"));
    let child;
    let devTools;

    try {
        const extraFlags = (process.env.F4_WEBGPU_BROWSER_FLAGS ?? "").split(/\s+/).filter(Boolean);
        child = spawn(
            browser,
            [
                "--headless=new",
                "--enable-gpu",
                "--enable-unsafe-webgpu",
                // Unquantized timestamp-query results (Chrome rounds them to
                // 100 us otherwise), so the GPU-timer check sees real pass times.
                "--enable-webgpu-developer-features",
                "--disable-extensions",
                "--no-first-run",
                "--no-default-browser-check",
                "--remote-allow-origins=*",
                "--remote-debugging-port=0",
                `--user-data-dir=${profile}`,
                "--window-size=1200,800",
                ...extraFlags,
                "about:blank",
            ],
            { stdio: ["ignore", "pipe", "pipe"], windowsHide: true },
        );
        let browserOutput = "";
        child.stdout.on("data", (data) => { browserOutput += data; });
        child.stderr.on("data", (data) => { browserOutput += data; });

        const portFile = join(profile, "DevToolsActivePort");
        await waitForFile(portFile, 10000);
        const port = Number.parseInt(readFileSync(portFile, "utf8").split(/\r?\n/)[0], 10);
        if (!Number.isInteger(port)) throw new Error(`Invalid DevTools port in ${portFile}`);

        const target = await waitForTarget(port, 10000);
        devTools = await DevTools.connect(target.webSocketDebuggerUrl);
        await devTools.call("Runtime.enable");
        await devTools.call("Page.enable");
        await devTools.call("Log.enable");
        // F4_WEBGPU_BENCH=<rounds>: run the point-light bench page instead of
        // the smoke and print its report.
        const benchRounds = Number.parseInt(process.env.F4_WEBGPU_BENCH ?? "0", 10) || 0;
        if (benchRounds > 0) {
            await devTools.call("Page.navigate", { url: `${url}?bench=${benchRounds}` });
            const benchDeadline = Date.now() + 900000;
            let bench;
            while (Date.now() < benchDeadline) {
                bench = await evaluateJson(devTools, `({
                    done: document.body?.dataset.f4BenchDone ?? null,
                    report: document.body?.dataset.f4Bench ?? null,
                    output: document.getElementById("output")?.textContent ?? null
                })`);
                if (bench.done !== null || bench.output?.includes("Aborted")) break;
                await delay(500);
            }
            console.log((bench?.report ?? "").split("|").join("\n"));
            if (bench?.done !== "true") {
                throw new Error(`WebGPU bench failed: ${JSON.stringify(bench)}\n` +
                                browserErrors(devTools.events).join("\n"));
            }
            return;
        }
        await devTools.call("Page.navigate", { url });

        const stateExpression = `({
            webgpu: !!navigator.gpu,
            ready: document.body?.dataset.f4Ready ?? null,
            draws: document.body?.dataset.f4Draws ?? null,
            noiseParity: document.body?.dataset.f4NoiseParity ?? null,
            retirement: document.body?.dataset.f4Retirement ?? null,
            pbrShaders: document.body?.dataset.f4PbrShaders ?? null,
            triangleStrip: document.body?.dataset.f4TriangleStrip ?? null,
            pbrVisual: document.body?.dataset.f4PbrVisual ?? null,
            renderer: document.body?.dataset.f4Renderer ?? null,
            rendererPixels: document.body?.dataset.f4RendererPixels ?? null,
            rendererHash: document.body?.dataset.f4RendererHash ?? null,
            pointLights: document.body?.dataset.f4PointLights ?? null,
            pointLightGain: document.body?.dataset.f4PointLightGain ?? null,
            pointShadows: document.body?.dataset.f4PointShadows ?? null,
            pointShadowLoss: document.body?.dataset.f4PointShadowLoss ?? null,
            bloom: document.body?.dataset.f4Bloom ?? null,
            bloomStats: document.body?.dataset.f4BloomStats ?? null,
            gpuTimers: document.body?.dataset.f4GpuTimers ?? null,
            gpuTimerStats: document.body?.dataset.f4GpuTimerStats ?? null,
            validationScene: document.body?.dataset.f4ValidationScene ?? null,
            sceneIndex: document.body?.dataset.f4SceneIndex ?? null,
            resizeCount: document.body?.dataset.f4ResizeCount ?? null,
            stableResize: document.body?.dataset.f4StableResize ?? null,
            validationErrors: document.body?.dataset.f4ValidationErrors ?? null,
            status: document.getElementById("status")?.textContent ?? null,
            output: document.getElementById("output")?.textContent ?? null
        })`;
        const deadline = Date.now() + 15000;
        let state;
        while (Date.now() < deadline) {
            state = await evaluateJson(devTools, stateExpression);
            if (state.output?.includes("Aborted") || state.ready === "true") break;
            await delay(100);
        }

        await delay(300);
        const errors = browserErrors(devTools.events);
        if (!state?.webgpu) throw new Error("navigator.gpu is unavailable; WebGL fallback is forbidden");
        if (state.ready !== "true") {
            throw new Error(
                `WebGPU app did not become ready: ${JSON.stringify(state)}\n` +
                `${errors.join("\n")}\n${browserOutput}`);
        }
        if (state.draws !== "130" || state.noiseParity !== "true" ||
            state.retirement !== "true" ||
            state.pbrShaders !== "true" ||
            state.triangleStrip !== "true" ||
            state.pbrVisual !== "true" ||
            state.renderer !== "true" ||
            Number.parseInt(state.rendererPixels, 10) <= 100 ||
            !/^[0-9a-f]{8}$/.test(state.rendererHash ?? "") ||
            state.pointLights !== "true" ||
            state.pointShadows !== "true" ||
            state.bloom !== "true" ||
            state.validationErrors !== "0") {
            throw new Error(`WebGPU conformance markers are invalid: ${JSON.stringify(state)}`);
        }
        if (state.output) errors.push(state.output.trim());
        if (errors.length !== 0) throw new Error(`Browser validation errors:\n${errors.join("\n")}`);

        // GPU timers: with timestamp-query on the adapter the app must have
        // timed the binning and main passes; without it, report unavailable.
        const timestampQuery = await evaluateJsonAsync(devTools, `(async () => {
            const adapter = await navigator.gpu.requestAdapter({ powerPreference: "high-performance" });
            return adapter ? adapter.features.has("timestamp-query") : false;
        })()`);
        const expectedTimers = timestampQuery ? "true" : "unavailable";
        if (state.gpuTimers !== expectedTimers) {
            throw new Error(
                `WebGPU GPU timers ${state.gpuTimers} (expected ${expectedTimers}): ${state.gpuTimerStats}`);
        }

        const initialResizeCount = Number.parseInt(state.resizeCount, 10);
        await devTools.call("Emulation.setDeviceMetricsOverride", {
            width: 1100,
            height: 760,
            deviceScaleFactor: 1,
            mobile: false,
        });
        const resizeDeadline = Date.now() + 10000;
        while (Date.now() < resizeDeadline) {
            state = await evaluateJson(devTools, stateExpression);
            const resized = Number.parseInt(state.resizeCount, 10) > initialResizeCount;
            if (resized && state.ready === "true" && state.stableResize === state.resizeCount) break;
            await delay(100);
        }
        if (Number.parseInt(state.resizeCount, 10) <= initialResizeCount ||
            state.ready !== "true" || state.stableResize !== state.resizeCount) {
            throw new Error(`WebGPU resize did not stabilize: ${JSON.stringify(state)}`);
        }
        await delay(300);
        const postResizeErrors = browserErrors(devTools.events);
        if (state.output) postResizeErrors.push(state.output.trim());
        if (postResizeErrors.length !== 0) {
            throw new Error(`Browser validation errors after resize:\n${postResizeErrors.join("\n")}`);
        }

        const canvasBounds = await evaluateJson(devTools, `(() => {
            const bounds = document.getElementById("canvas").getBoundingClientRect();
            return { x: bounds.x, y: bounds.y, width: bounds.width, height: bounds.height };
        })()`);
        const auditCapture = await devTools.call("Page.captureScreenshot", {
            format: "png",
            captureBeyondViewport: false,
            clip: { ...canvasBounds, scale: 1 },
        });
        const auditUrl = `data:image/png;base64,${auditCapture.data}`;
        const visual = await evaluateJsonAsync(devTools, `(async () => {
            const response = await fetch(${JSON.stringify(auditUrl)});
            const bitmap = await createImageBitmap(await response.blob());
            const target = new OffscreenCanvas(bitmap.width, bitmap.height);
            const context = target.getContext("2d", { willReadFrequently: true });
            context.drawImage(bitmap, 0, 0);
            bitmap.close();
            const pixels = context.getImageData(0, 0, target.width, target.height).data;
            const backgroundOffset = (5 * target.width + 5) * 4;
            const background = [pixels[backgroundOffset], pixels[backgroundOffset + 1],
                                pixels[backgroundOffset + 2]];
            const bins = new Set();
            let foreground = 0;
            let minX = target.width;
            let minY = target.height;
            let maxX = -1;
            let maxY = -1;
            for (let y = 0; y < target.height; ++y) {
                for (let x = 0; x < target.width; ++x) {
                    const offset = (y * target.width + x) * 4;
                    const difference = Math.abs(pixels[offset] - background[0]) +
                        Math.abs(pixels[offset + 1] - background[1]) +
                        Math.abs(pixels[offset + 2] - background[2]);
                    if (difference <= 36 || pixels[offset + 3] !== 255) continue;
                    ++foreground;
                    minX = Math.min(minX, x);
                    minY = Math.min(minY, y);
                    maxX = Math.max(maxX, x);
                    maxY = Math.max(maxY, y);
                    bins.add(
                        (pixels[offset] >> 5) + ":" +
                        (pixels[offset + 1] >> 5) + ":" +
                        (pixels[offset + 2] >> 5));
                }
            }
            return {
                width: target.width,
                height: target.height,
                foreground,
                foregroundRatio: foreground / (target.width * target.height),
                boundsWidth: maxX >= minX ? maxX - minX + 1 : 0,
                boundsHeight: maxY >= minY ? maxY - minY + 1 : 0,
                colorBins: bins.size,
            };
        })()`);
        if (visual.foregroundRatio < 0.03 || visual.foregroundRatio > 0.4 ||
            visual.boundsWidth < visual.width * 0.2 ||
            visual.boundsHeight < visual.height * 0.2 || visual.colorBins < 3) {
            throw new Error(`WebGPU PBR cube pixel audit failed: ${JSON.stringify(visual)}`);
        }

        mkdirSync(captureDirectory, { recursive: true });
        const rendererPixels = await evaluateJson(devTools, `(() => {
            const address = Module._f4RendererCaptureData();
            const size = Module._f4RendererCaptureSize();
            return Array.from(HEAPU8.subarray(address, address + size));
        })()`);
        if (rendererPixels.length !== 64 * 64 * 4) {
            throw new Error(`WebGPU renderer capture has invalid size ${rendererPixels.length}`);
        }
        const rendererBytes = Buffer.from(rendererPixels);
        let rendererFnv = 0x811c9dc5;
        for (const value of rendererBytes) rendererFnv = Math.imul(rendererFnv ^ value, 0x01000193);
        const rendererHash = (rendererFnv >>> 0).toString(16).padStart(8, "0");
        if (rendererHash !== state.rendererHash) {
            throw new Error(`WebGPU renderer capture hash mismatch: ${rendererHash} != ${state.rendererHash}`);
        }
        const rendererReference = inflateSync(Buffer.from(m11ReferenceDeflateBase64, "base64"));
        if (rendererReference.length !== rendererBytes.length) {
            throw new Error(`M11 reference size ${rendererReference.length} != ${rendererBytes.length}`);
        }
        let parityMismatchPixels = 0;
        let parityMaxChannelDelta = 0;
        for (let pixel = 0; pixel < 64 * 64; ++pixel) {
            let mismatch = false;
            for (let channel = 0; channel < 4; ++channel) {
                const index = pixel * 4 + channel;
                const delta = Math.abs(rendererBytes[index] - rendererReference[index]);
                parityMaxChannelDelta = Math.max(parityMaxChannelDelta, delta);
                if (delta > 2) mismatch = true;
            }
            if (mismatch) ++parityMismatchPixels;
        }
        if (parityMismatchPixels !== 0) {
            throw new Error(
                `M11 WebGPU full-frame parity failed: ${parityMismatchPixels} pixels, ` +
                `max channel delta ${parityMaxChannelDelta}`,
            );
        }
        writeFileSync(join(captureDirectory, "renderer.rgba"), rendererBytes);

        // Point-light shadow and bloom frames, held to the references the
        // native OpenGL / Vulkan / DX12 test locks (tests/engine/test_point_lights.cpp).
        const effectSummaries = [];
        for (const [which, name] of [[0, "point_shadow"], [1, "bloom"]]) {
            const effectPixels = await evaluateJson(devTools, `(() => {
                const address = Module._f4EffectCaptureData(${which});
                return Array.from(HEAPU8.subarray(address, address + 64 * 64 * 4));
            })()`);
            const effectBytes = Buffer.from(effectPixels);
            const effectReference = readFileSync(
                new URL(`../tests/fixtures/renderer_${name}_reference.rgba`, import.meta.url));
            if (effectReference.length !== effectBytes.length) {
                throw new Error(`${name} reference size ${effectReference.length} != ${effectBytes.length}`);
            }
            let effectMismatch = 0;
            let effectMaxDelta = 0;
            for (let pixel = 0; pixel < 64 * 64; ++pixel) {
                let mismatch = false;
                for (let channel = 0; channel < 4; ++channel) {
                    const index = pixel * 4 + channel;
                    const delta = Math.abs(effectBytes[index] - effectReference[index]);
                    effectMaxDelta = Math.max(effectMaxDelta, delta);
                    if (delta > 2) mismatch = true;
                }
                if (mismatch) ++effectMismatch;
            }
            writeFileSync(join(captureDirectory, `renderer_${name}.rgba`), effectBytes);
            if (effectMismatch !== 0) {
                throw new Error(
                    `WebGPU ${name} parity failed: ${effectMismatch} pixels, max channel delta ${effectMaxDelta}`);
            }
            effectSummaries.push(`${name} max=${effectMaxDelta}`);
        }
        const hashes = [];
        for (let frame = 1; frame <= 3; ++frame) {
            const capture = await devTools.call("Page.captureScreenshot", {
                format: "png",
                captureBeyondViewport: false,
            });
            const bytes = Buffer.from(capture.data, "base64");
            writeFileSync(join(captureDirectory, `frame-${frame}.png`), bytes);
            hashes.push(createHash("sha256").update(bytes).digest("hex"));
            await delay(200);
        }
        if (new Set(hashes).size !== 1) {
            throw new Error(`Repeated WebGPU captures are unstable: ${hashes.join(", ")}`);
        }

        const validationScenes = [
            { index: 1, slug: "pbr-spheres" },
            { index: 2, slug: "mipmapping" },
            { index: 3, slug: "z-fighting" },
            { index: 4, slug: "alpha-order" },
            { index: 5, slug: "normal-map" },
            { index: 6, slug: "hybrid" },
            { index: 7, slug: "cornell" },
            { index: 8, slug: "terrain" },
        ];
        const validationSummaries = [];
        for (const scene of validationScenes) {
            const switched = await evaluateJsonAsync(
                devTools, `Module._f4SetValidationScene(${scene.index})`);
            if (switched !== 1) {
                state = await evaluateJson(devTools, stateExpression);
                const switchErrors = browserErrors(devTools.events);
                if (state.output) switchErrors.push(state.output.trim());
                throw new Error(
                    `Failed to initialize ${scene.slug}: ${JSON.stringify(state)}\n` +
                    switchErrors.join("\n"),
                );
            }
            const sceneDeadline = Date.now() + 10000;
            while (Date.now() < sceneDeadline) {
                state = await evaluateJson(devTools, stateExpression);
                if (state.ready === "true" && state.validationScene === "true" &&
                    state.sceneIndex === String(scene.index)) break;
                await delay(100);
            }
            if (state.ready !== "true" || state.validationScene !== "true" ||
                state.sceneIndex !== String(scene.index) || state.validationErrors !== "0") {
                throw new Error(`WebGPU validation scene did not stabilize: ${JSON.stringify(state)}`);
            }
            const sceneErrors = browserErrors(devTools.events);
            if (state.output) sceneErrors.push(state.output.trim());
            if (sceneErrors.length !== 0) {
                throw new Error(`${scene.slug} emitted browser errors:\n${sceneErrors.join("\n")}`);
            }

            const sceneHashes = [];
            let firstCaptureData;
            for (let frame = 1; frame <= 3; ++frame) {
                const capture = await devTools.call("Page.captureScreenshot", {
                    format: "png",
                    captureBeyondViewport: false,
                    clip: { ...canvasBounds, scale: 1 },
                });
                firstCaptureData ??= capture.data;
                const bytes = Buffer.from(capture.data, "base64");
                writeFileSync(
                    join(captureDirectory, `validation-${scene.index}-${scene.slug}-${frame}.png`),
                    bytes,
                );
                sceneHashes.push(createHash("sha256").update(bytes).digest("hex"));
                await delay(150);
            }
            if (new Set(sceneHashes).size !== 1) {
                throw new Error(`${scene.slug} captures blinked: ${sceneHashes.join(", ")}`);
            }

            const sceneCaptureUrl = `data:image/png;base64,${firstCaptureData}`;
            const sceneVisual = await evaluateJsonAsync(devTools, `(async () => {
                const response = await fetch(${JSON.stringify(sceneCaptureUrl)});
                const bitmap = await createImageBitmap(await response.blob());
                const target = new OffscreenCanvas(bitmap.width, bitmap.height);
                const context = target.getContext("2d", { willReadFrequently: true });
                context.drawImage(bitmap, 0, 0);
                bitmap.close();
                const pixels = context.getImageData(0, 0, target.width, target.height).data;
                const bins = new Set();
                let opaque = 0;
                let minLuma = 255;
                let maxLuma = 0;
                for (let offset = 0; offset < pixels.length; offset += 4) {
                    if (pixels[offset + 3] === 255) ++opaque;
                    const luma = Math.round(
                        pixels[offset] * 0.2126 + pixels[offset + 1] * 0.7152 +
                        pixels[offset + 2] * 0.0722);
                    minLuma = Math.min(minLuma, luma);
                    maxLuma = Math.max(maxLuma, luma);
                    bins.add(
                        (pixels[offset] >> 4) + ":" +
                        (pixels[offset + 1] >> 4) + ":" +
                        (pixels[offset + 2] >> 4));
                }
                return {
                    colorBins: bins.size,
                    lumaRange: maxLuma - minLuma,
                    opaqueRatio: opaque / (target.width * target.height),
                };
            })()`);
            if (sceneVisual.colorBins < 4 || sceneVisual.lumaRange < 20 ||
                sceneVisual.opaqueRatio < 0.99) {
                throw new Error(`${scene.slug} pixel audit failed: ${JSON.stringify(sceneVisual)}`);
            }
            validationSummaries.push(
                `${scene.slug}:${sceneVisual.colorBins}/${sceneVisual.lumaRange}`);
        }
        console.log(
            `WebGPU ready; renderer=${state.rendererPixels} pixels/${state.rendererHash}; ` +
            `point-light red/other gain=${state.pointLightGain}; ` +
            `point-shadow red loss/brighter=${state.pointShadowLoss}; ` +
            `bloom zero-delta/glow pixels=${state.bloomStats}; ` +
            `GPU timers ${state.gpuTimers} (${state.gpuTimerStats}); ` +
            `M11 parity max=${parityMaxChannelDelta}; ` +
            `effect parity ${effectSummaries.join(", ")}; ` +
            `PBR pixels=${visual.foreground}; colors=${visual.colorBins}; ` +
            `${validationScenes.length} demo scenes stable ` +
            `(${validationSummaries.join(", ")}); ` +
            `sha256=${hashes[0]}`,
        );
    } finally {
        devTools?.close();
        if (child && child.exitCode === null) {
            if (process.platform === "win32") {
                spawnSync("taskkill", ["/pid", String(child.pid), "/t", "/f"], { windowsHide: true });
            } else {
                child.kill("SIGTERM");
            }
        }
        await new Promise((resolveClose) => server.close(resolveClose));
        await removeProfile(profile);
    }
}

run().catch((error) => {
    console.error(error.stack ?? error);
    process.exitCode = 1;
});
