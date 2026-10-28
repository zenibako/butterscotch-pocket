# Pocket Sync plugin icons

## Size for the consumer

Keep this port's `src/openfpga/tools/pocket-data/plugin_icon.png` at **40×40
pixels**, retaining the artwork and transparency. This is a workaround for
Pocket Sync's compact core-detail plugin row, not an official plugin schema
limit. A large PNG can be valid and still expand the row.

In Pocket Sync v7.1.1, the compact row renders `logo_url` without bounding the
image dimensions; the full Plugins screen separately sets 120×120 pixels.
Do not judge the fix from the full screen alone or enlarge the release PNG
because that screen displays it larger. Recheck the consumer when it changes:

- [Compact component](https://github.com/neil-morrison44/pocket-sync/blob/40f09811a9e7631f720b7aaa206ba686056d5449/src/components/plugins/miniItem/index.tsx)
- [Compact layout](https://github.com/neil-morrison44/pocket-sync/blob/40f09811a9e7631f720b7aaa206ba686056d5449/src/components/plugins/miniItem/index.css)
- [Full Plugins layout](https://github.com/neil-morrison44/pocket-sync/blob/40f09811a9e7631f720b7aaa206ba686056d5449/src/components/plugins/index.css)
- [Plugin specification](https://github.com/openfpga-library/pocket-plugin/blob/39dae308f7db00d77a4314fc76ad5d8d5b09b81b/README.md)

## Check all logos and both delivery paths

1. Enumerate `plugin.json` manifests and `logo_url` references. Pocket Sync v7.1.1
   discovers suggestions from cores' `updaters.json` `plugins` arrays and
   installed plugins from `pocket-sync.json` `config.plugins`; it also accepts
   manually entered GitHub repositories. There is no exhaustive central
   catalog. Read available card/config metadata and report the scope,
   revision/date, count and any unavailable URLs. See
   [discovery UI](https://github.com/neil-morrison44/pocket-sync/blob/40f09811a9e7631f720b7aaa206ba686056d5449/src/components/plugins/index.tsx)
   and [selectors](https://github.com/neil-morrison44/pocket-sync/blob/40f09811a9e7631f720b7aaa206ba686056d5449/src/jotai/plugins/selectors.ts).
2. Inspect each resolved image's intrinsic width/height, type and byte size.
   Follow redirects: a local PNG change does not change a published URL.
   On macOS, use `sips -g pixelWidth -g pixelHeight path/to/icon.png`.
3. For this port, check the tracked PNG and the asset resolved by
   `plugin.json`'s `logo_url`, currently the latest stable release download.
   Verify the downloaded bytes/dimensions rather than assuming the local fix
   is live. Preserve published release assets and their checksums; use the
   explicitly authorized next complete release to deliver changes.
4. Reproduce the compact row with the actual image and client CSS, and inspect
   the full Plugins screen. Check row bounds and horizontal overflow at a
   realistic viewport. For this port, a 40×40 icon produced a 56px compact row
   in the v7.1.1 layout fixture; this is fixture evidence, not a guarantee for
   every client version. Record the screenshot and source revision.
5. After publication, verify the latest-release redirect, asset hash and
   dimensions. If the client still shows old artwork, check its resolved URL
   and reload/cache behavior before changing the source again.

Keep third-party catalog audits read-only unless changes are separately
authorized. Preserve original artwork when resizing. Do not apply plugin PNG
sizes to the Pocket core's binary icon, menu banner, screenshots or game assets.
Use `pocket-release` for complete release validation and publication rules.
