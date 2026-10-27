import * as assert from "assert";
import * as path from "path";
import {
  getServerCapabilities,
  parseServerCapabilities,
} from "../../serverCapabilities";

suite("Server Capabilities Test Suite", () => {
  test("requires an explicit reporting capability from the selected binary", () => {
    assert.deepStrictEqual(parseServerCapabilities('{"crashReporting":true}'), {
      crashReporting: true,
    });
    for (const output of [
      '{"crashReporting":false}',
      '{"crashReporting":"true"}',
      '{"crashReporting":1}',
      "{}",
      "null",
      "old server help output",
    ]) {
      assert.deepStrictEqual(parseServerCapabilities(output), {
        crashReporting: false,
      });
    }
  });

  test("unavailable binaries cannot enable reporting or its suggestion", async () => {
    const capabilities = await getServerCapabilities(
      path.join(__dirname, "nonexistent-server"),
    );
    assert.strictEqual(capabilities.crashReporting, false);
  });
});
