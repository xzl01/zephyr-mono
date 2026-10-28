#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Execute the shipped inline RSS manager with a minimal DOM and fake fetch.

No browser/network dependency is required; layout is separately rendered in the
native/desktop preview workflow. These checks target real event-handler state.
"""
from pathlib import Path
import os
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
HTML = (ROOT / "src/web/content-center.html").read_text()

NODE = r'''
const assert = require("node:assert/strict");
const vm = require("node:vm");
const script = require("node:fs").readFileSync(0, "utf8");
const tick = () => new Promise(resolve => setImmediate(resolve));

class Element {
  constructor(tag = "div") {
    this.tagName = tag.toUpperCase(); this.children = []; this.listeners = new Map();
    this.hidden = false; this.disabled = false; this.value = ""; this.textContent = "";
    this.attributes = {}; this.classes = new Set();
    this.classList = { toggle: (name, yes) => yes ? this.classes.add(name) : this.classes.delete(name), contains: name => this.classes.has(name) };
  }
  append(...children) { this.children.push(...children); }
  replaceChildren(...children) { this.children = [...children]; }
  addEventListener(name, handler) {
    if (!this.listeners.has(name)) this.listeners.set(name, []);
    this.listeners.get(name).push(handler);
  }
  setAttribute(name, value) { this.attributes[name] = value; }
  focus() { this.focused = true; }
  get lastElementChild() { return this.children.at(-1); }
  querySelector(selector) {
    const tag = selector.toUpperCase();
    for (const child of this.children) {
      if (child.tagName === tag) return child;
      const match = child.querySelector(selector); if (match) return match;
    }
    return null;
  }
  async fire(type) {
    if (this.disabled && type !== "submit") return;
    for (const fn of this.listeners.get(type) || []) await fn({ preventDefault() {}, submitter: this });
    await tick();
  }
}

async function harness(options = {}) {
  const elements = new Map();
  const get = selector => {
    if (!elements.has(selector)) elements.set(selector, new Element(selector.includes("form") ? "form" : "div"));
    return elements.get(selector);
  };
  get("#panel").hidden = true;
  get("#rss-add").disabled = get("#rss-save").disabled = true;
  const state = {
    calls: [], saved: [], confirm: true,
    rssGet: { status: 200, body: { max_sources: 4, sources: [{ id: "gold", name: "金价", url: "http://example.test/gold.xml" }] } },
    rssPut: { status: 200, body: { ok: true } },
    content: { status: 200, body: [] },
  };
  if (options.rssStatus) state.rssGet = { status: options.rssStatus, body: { message: "not available" } };
  const storage = new Map();
  if (options.token) storage.set("papermono-session-v1", options.token);
  const context = vm.createContext({
    document: { querySelector: get, createElement: tag => new Element(tag) },
    localStorage: { getItem: key => storage.get(key) || "", setItem: (key, value) => storage.set(key, value), removeItem: key => storage.delete(key) },
    location: { hash: "", pathname: "/", search: "", hostname: "papermono.test", replace() {} },
    history: { replaceState() {} },
    Headers, URL, TextEncoder, Math, Date, setTimeout, clearTimeout, console,
    btoa: value => Buffer.from(value, "binary").toString("base64"),
    confirm: () => state.confirm,
    fetch: async (path, request = {}) => {
      state.calls.push({ path, ...request });
      const method = request.method || "GET";
      let response;
      if (path === "/api/v1/rss/sources" && method === "PUT") {
        state.saved.push(JSON.parse(request.body)); response = state.rssPut;
      } else if (path === "/api/v1/rss/sources") response = state.rssGet;
      else if (path === "/api/v1/content") response = state.content;
      else throw new Error(`unexpected fetch ${path}`);
      if (response.wait) await response.wait;
      if (response.error) throw new Error(response.error);
      const text = JSON.stringify(response.body);
      return { status: response.status, ok: response.status >= 200 && response.status < 300,
        text: async () => text, json: async () => JSON.parse(text) };
    },
  });
  await vm.runInContext(script, context);
  return { state, get, context, storage,
    eval: expression => vm.runInContext(expression, context),
    rows: () => get("#rss-list").children.filter(child => child.className === "rss-source") };
}

function inputs(row) { return row.children.flatMap(label => label.children).filter(child => child.tagName === "INPUT"); }
function remove(row) { return row.children.find(child => child.tagName === "BUTTON"); }
async function edit(input, value) { input.value = value; await input.fire("input"); }
async function loaded() {
  const h = await harness(); h.eval('storeToken("session-test")');
  await h.eval("refreshRssSources()"); return h;
}

(async () => {
  let h = await harness();
  assert.equal(h.get("#rss-add").disabled, true);
  assert.equal(h.get("#rss-save").disabled, true);
  await h.get("#rss-form").fire("submit");
  assert.equal(h.state.saved.length, 0, "cannot erase sources before loading");

  h = await loaded();
  assert.equal(h.rows().length, 1); assert.equal(inputs(h.rows()[0])[0].value, "金价");
  assert.equal(h.get("#rss-save").disabled, false);
  /* A slow reload must not overwrite edits/additions/removals made after
   * its GET started. Saving waits until that read has settled. */
  for (const operation of ["edit", "add", "remove"]) {
    const delayed = await loaded();
    let release;
    delayed.state.rssGet.wait = new Promise(resolve => { release = resolve; });
    const reading = delayed.eval("refreshRssSources()");
    await tick();
    assert.equal(delayed.get("#rss-save").disabled, true);
    if (operation === "edit") await edit(inputs(delayed.rows()[0])[0], "读取期间新草稿");
    else if (operation === "add") await delayed.get("#rss-add").fire("click");
    else await remove(delayed.rows()[0]).fire("click");
    release(); await reading;
    assert.equal(delayed.eval("rssDirty"), true);
    assert.match(delayed.get("#rss-message").textContent, /保留/);
    if (operation === "edit") assert.equal(inputs(delayed.rows()[0])[0].value, "读取期间新草稿");
    else assert.equal(delayed.rows().length, operation === "add" ? 2 : 0);
    assert.equal(delayed.get("#rss-save").disabled, false);
  }
  assert.equal(h.state.calls[0].headers.get("X-PaperMono-Session"), "session-test");
  await edit(inputs(h.rows()[0])[0], "  更新金价  ");
  await h.get("#rss-form").fire("submit");
  assert.equal(h.state.saved.at(-1).sources[0].name, "更新金价");
  assert.equal(h.get("#rss-fields").disabled, false);
  assert.equal(h.get("#rss-form").attributes["aria-busy"], "false");
  /* A successful save replaces the source objects. Input handlers must have
   * been rebound to those objects rather than closing over the previous edit. */
  await edit(inputs(h.rows()[0])[0], "第二次编辑");
  await edit(inputs(h.rows()[0])[1], "http://example.test/second.xml");
  await h.get("#rss-form").fire("submit");
  assert.equal(h.state.saved.at(-1).sources[0].name, "第二次编辑");
  assert.equal(h.state.saved.at(-1).sources[0].url, "http://example.test/second.xml");

  /* Add/remove follows the live list, enforces four sources, and can save an
   * intentional empty list after sources were successfully loaded. */
  for (let i = 1; i < 4; ++i) {
    await h.get("#rss-add").fire("click");
    const row = h.rows().at(-1);
    await edit(inputs(row)[0], `栏目${i}`);
    await edit(inputs(row)[1], `http://example.test/${i}.xml`);
  }
  assert.equal(h.rows().length, 4); assert.equal(h.get("#rss-add").disabled, true);
  await h.get("#rss-add").fire("click"); assert.equal(h.rows().length, 4);
  await remove(h.rows().at(-1)).fire("click");
  assert.equal(h.rows().length, 3); assert.equal(h.get("#rss-add").disabled, false);
  await h.get("#rss-form").fire("submit"); assert.equal(h.state.saved.at(-1).sources.length, 3);
  while (h.rows().length) await remove(h.rows()[0]).fire("click");
  await h.get("#rss-form").fire("submit"); assert.equal(h.state.saved.at(-1).sources.length, 0);

  h = await loaded();
  await edit(inputs(h.rows()[0])[0], "未保存编辑");
  h.state.rssPut = { status: 503, body: { message: "TF 无法写入" } };
  await h.get("#rss-form").fire("submit");
  assert.equal(inputs(h.rows()[0])[0].value, "未保存编辑");
  assert.equal(h.eval("rssDirty"), true);
  assert.match(h.get("#rss-message").textContent, /保存失败.*TF 无法写入/);
  assert.equal(h.get("#rss-fields").disabled, false);
  const getCount = h.state.calls.filter(call => call.path === "/api/v1/rss/sources" && !call.method).length;
  await h.get("#refresh-button").fire("click");
  assert.equal(inputs(h.rows()[0])[0].value, "未保存编辑");
  assert.equal(h.eval("rssDirty"), true);
  assert.equal(h.state.calls.filter(call => call.path === "/api/v1/rss/sources" && !call.method).length, getCount);
  h.state.confirm = false;
  await h.get("#rss-reload").fire("click");
  assert.equal(inputs(h.rows()[0])[0].value, "未保存编辑");
  h.state.confirm = true;
  await h.get("#rss-reload").fire("click");
  assert.equal(inputs(h.rows()[0])[0].value, "金价"); assert.equal(h.eval("rssDirty"), false);

  h = await loaded();
  await edit(inputs(h.rows()[0])[0], "长".repeat(16));
  await h.get("#rss-form").fire("submit"); assert.equal(h.state.saved.length, 0);
  await edit(inputs(h.rows()[0])[0], "长".repeat(15));
  await edit(inputs(h.rows()[0])[1], "https://example.test/rss");
  await h.get("#rss-form").fire("submit"); assert.equal(h.state.saved.length, 0);
  await edit(inputs(h.rows()[0])[1], "http://example.test/rss");
  await h.get("#rss-form").fire("submit"); assert.equal(h.state.saved.length, 1);

  /* Loading failure must not enable a save which would replace stored sources
   * with the initial empty browser list. Old firmware can omit the feature. */
  h = await harness({ rssStatus: 503 }); h.eval('storeToken("session-test")');
  await h.eval("refreshRssSources()"); await h.get("#rss-form").fire("submit");
  assert.equal(h.get("#rss-save").disabled, true); assert.equal(h.state.saved.length, 0);
  h = await harness({ rssStatus: 404, token: "existing-session" });
  assert.equal(h.get("#rss-panel").hidden, true);
  assert.equal(h.get("#panel").hidden, false);

  /* RSS GET can be the first request to notice an expired session. Restore
   * must not swallow its 401 and reopen the paired panel afterward. */
  h = await harness({ rssStatus: 401, token: "expired-session" });
  assert.equal(h.get("#pair-panel").hidden, false);
  assert.equal(h.get("#panel").hidden, true);
  assert.equal(h.storage.has("papermono-session-v1"), false);
  console.log("RSS sources web: live edit/save, limits, preserved drafts, recovery and session states: PASS");
})().catch(error => { console.error(error); process.exitCode = 1; });
'''


class RssSourcesWebTests(unittest.TestCase):
    def test_real_inline_script(self):
        scripts = re.findall(r"<script(?:\s[^>]*)?>(.*?)</script>", HTML, re.S | re.I)
        self.assertEqual(len(scripts), 1)
        subprocess.run([os.environ.get("NODE", "node"), "-e", NODE], input=scripts[0], text=True, check=True)


if __name__ == "__main__":
    unittest.main()
