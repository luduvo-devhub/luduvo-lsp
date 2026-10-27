import { execFile } from "child_process";
import { promisify } from "util";

export type ServerCapabilities = { crashReporting: boolean };

const execFileAsync = promisify(execFile);

export function parseServerCapabilities(output: string): ServerCapabilities {
  try {
    const capabilities: unknown = JSON.parse(output);
    return {
      crashReporting:
        typeof capabilities === "object" &&
        capabilities !== null &&
        "crashReporting" in capabilities &&
        capabilities.crashReporting === true,
    };
  } catch {
    return { crashReporting: false };
  }
}

export async function getServerCapabilities(
  serverPath: string,
): Promise<ServerCapabilities> {
  try {
    const { stdout } = await execFileAsync(
      serverPath,
      ["--show-capabilities"],
      {
        timeout: 2_000,
        windowsHide: true,
      },
    );
    return parseServerCapabilities(stdout);
  } catch {
    // Older/unavailable binaries can't safely advertise crash reporting
    return { crashReporting: false };
  }
}
