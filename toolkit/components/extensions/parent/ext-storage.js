/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

ChromeUtils.defineESModuleGetters(this, {
  AddonManagerPrivate: "resource://gre/modules/AddonManager.sys.mjs",
  ExtensionStorage: "resource://gre/modules/ExtensionStorage.sys.mjs",
  ExtensionStorageIDB: "resource://gre/modules/ExtensionStorageIDB.sys.mjs",
  NativeManifests: "resource://gre/modules/NativeManifests.sys.mjs",
  extensionStorageSession: "resource://gre/modules/ExtensionStorage.sys.mjs",
});

var { ExtensionError } = ExtensionUtils;
var { ignoreEvent } = ExtensionCommon;

ChromeUtils.defineLazyGetter(this, "extensionStorageSync", () => {
  // TODO bug 1637465: Remove Kinto-based implementation.
  if (Services.prefs.getBoolPref("webextensions.storage.sync.kinto")) {
    const { extensionStorageSyncKinto } = ChromeUtils.importESModule(
      "resource://gre/modules/ExtensionStorageSyncKinto.sys.mjs"
    );
    return extensionStorageSyncKinto;
  }

  const { extensionStorageSync } = ChromeUtils.importESModule(
    "resource://gre/modules/ExtensionStorageSync.sys.mjs"
  );
  return extensionStorageSync;
});

XPCOMUtils.defineLazyPreferenceGetter(
  this,
  "prefStorageSyncEnabled",
  "webextensions.storage.sync.enabled",
  true
);

const enforceNoTemporaryAddon = extensionId => {
  const EXCEPTION_MESSAGE =
    "The storage API will not work with a temporary addon ID. " +
    "Please add an explicit addon ID to your manifest. " +
    "For more information see https://mzl.la/3lPk1aE.";
  if (AddonManagerPrivate.isTemporaryInstallID(extensionId)) {
    throw new ExtensionError(EXCEPTION_MESSAGE);
  }
};

// Set of extensions that already recorded the sync quota event.
const syncQuotasRecorded = new WeakSet();
const QUOTA_BYTES_PER_ITEM = 8_192;

async function recordSyncQuotaTelemetry(extension, context) {
  if (syncQuotasRecorded.has(extension) || !prefStorageSyncEnabled) {
    return;
  }
  syncQuotasRecorded.add(extension);
  let items = await extensionStorageSync.get(extension, null, context);
  let items_over_quota = 0;
  let total_size_bytes = 0;
  let entries = Object.entries(items);
  for (let [key, value] of entries) {
    let bytes = JSON.stringify(value).length;
    total_size_bytes += key.length + bytes;
    if (bytes > QUOTA_BYTES_PER_ITEM) {
      items_over_quota++;
    }
  }
  Glean.extensionsData.syncUsageQuotas.record({
    addon_id: extension.id,
    total_size_bytes,
    items_count: entries.length,
    items_over_quota,
    backend: extensionStorageSync.backend,
  });
}

// WeakMap[extension -> Promise<SerializableMap?>]
const managedStorage = new WeakMap();

const lookupManagedStorage = async (extensionId, context) => {
  if (Services.policies) {
    let extensionPolicy = Services.policies.getExtensionPolicy(extensionId);
    if (extensionPolicy) {
      return ExtensionStorage._serializableMap(extensionPolicy);
    }
  }
  let info = await NativeManifests.lookupManifest(
    "storage",
    extensionId,
    context
  );
  if (info) {
    return ExtensionStorage._serializableMap(info.manifest.data);
  }
  return null;
};

const getManagedStorageManifestData = async (extension, context) => {
  let lookup = managedStorage.get(extension);

  if (!lookup) {
    lookup = await lookupManagedStorage(extension.id, context);
    managedStorage.set(extension, lookup);
  }

  if (!lookup) {
    throw new ExtensionError("Managed storage manifest not found");
  }

  return lookup;
};

this.storage = class extends ExtensionAPIPersistent {
  constructor(extension) {
    super(extension);

    const messageName = `Extension:StorageLocalOnChanged:${extension.uuid}`;
    Services.ppmm.addMessageListener(messageName, this);
    this.clearStorageChangedListener = () => {
      Services.ppmm.removeMessageListener(messageName, this);
    };
  }

  PERSISTENT_EVENTS = {
    onChanged({ context, fire }) {
      let unregisterLocal = this.registerLocalChangedListener(changes => {
        // |changes| is already serialized. Send the raw value, so that it can
        // be deserialized by the onChanged handler in child/ext-storage.js.
        fire.raw(changes, "local");
      });

      // Session storage is not exposed to content scripts, and `context` does
      // not exist while setting up persistent listeners for an event page.
      let unregisterSession;
      if (
        !context ||
        context.envType === "addon_parent" ||
        context.envType === "devtools_parent"
      ) {
        unregisterSession = extensionStorageSession.registerListener(
          this.extension,
          changes => fire.async(changes, "session")
        );
      }

      let unregisterSync = this.registerSyncChangedListener(changes => {
        fire.async(changes, "sync");
      });

      return {
        unregister() {
          unregisterLocal();
          unregisterSession?.();
          unregisterSync();
        },
        convert(_fire) {
          fire = _fire;
        },
      };
    },
    "local.onChanged"({ fire }) {
      let unregister = this.registerLocalChangedListener(changes => {
        // |changes| is already serialized. Send the raw value, so that it can
        // be deserialized by the onChanged handler in child/ext-storage.js.
        fire.raw(changes);
      });
      return {
        unregister,
        convert(_fire) {
          fire = _fire;
        },
      };
    },
    "session.onChanged"({ fire }) {
      let unregister = extensionStorageSession.registerListener(
        this.extension,
        changes => fire.async(changes)
      );

      return {
        unregister,
        convert(_fire) {
          fire = _fire;
        },
      };
    },
    "sync.onChanged"({ fire }) {
      let unregister = this.registerSyncChangedListener(changes => {
        fire.async(changes);
      });
      return {
        unregister,
        convert(_fire) {
          fire = _fire;
        },
      };
    },
  };

  registerLocalChangedListener(onStorageLocalChanged) {
    const extensionId = this.extension.id;
    ExtensionStorage.addOnChangedListener(extensionId, onStorageLocalChanged);
    ExtensionStorageIDB.addOnChangedListener(
      extensionId,
      onStorageLocalChanged
    );
    return () => {
      ExtensionStorage.removeOnChangedListener(
        extensionId,
        onStorageLocalChanged
      );
      ExtensionStorageIDB.removeOnChangedListener(
        extensionId,
        onStorageLocalChanged
      );
    };
  }

  registerSyncChangedListener(onStorageSyncChanged) {
    const { extension } = this;
    let closeCallback;
    // The ExtensionStorageSyncKinto implementation of addOnChangedListener
    // relies on context.callOnClose (via ExtensionStorageSync.registerInUse)
    // to keep track of active users of the storage. We don't need to pass a
    // real BaseContext instance, a dummy object with the callOnClose method
    // works too. This enables us to register a primed listener before any
    // context is available.
    // TODO bug 1637465: Remove this when the Kinto backend is dropped.
    let dummyContextForKinto = {
      callOnClose({ close }) {
        closeCallback = close;
      },
    };
    extensionStorageSync.addOnChangedListener(
      extension,
      onStorageSyncChanged,
      dummyContextForKinto
    );
    return () => {
      extensionStorageSync.removeOnChangedListener(
        extension,
        onStorageSyncChanged
      );
      // May be void if ExtensionStorageSyncKinto.sys.mjs was not used.
      // ExtensionStorageSync.sys.mjs does not use the context.
      closeCallback?.();
    };
  }

  onShutdown() {
    const { clearStorageChangedListener } = this;
    this.clearStorageChangedListener = null;

    if (clearStorageChangedListener) {
      clearStorageChangedListener();
    }
  }

  receiveMessage({ name, data }) {
    if (name !== `Extension:StorageLocalOnChanged:${this.extension.uuid}`) {
      return;
    }

    ExtensionStorageIDB.notifyListeners(this.extension.id, data);
  }

  getAPI(context) {
    let { extension } = context;

    return {
      storage: {
        local: {
          async callMethodInParentProcess(method, args) {
            const res = await ExtensionStorageIDB.selectBackend({ extension });
            if (!res.backendEnabled) {
              return ExtensionStorage[method](extension.id, ...args);
            }

            const persisted = extension.hasPermission("unlimitedStorage");
            const db = await ExtensionStorageIDB.open(
              res.storagePrincipal.deserialize(this, true),
              persisted
            );
            try {
              const changes = await db[method](...args);
              if (changes) {
                ExtensionStorageIDB.notifyListeners(extension.id, changes);
              }
              return changes;
            } catch (err) {
              const normalizedError = ExtensionStorageIDB.normalizeStorageError(
                {
                  error: err,
                  extensionId: extension.id,
                  storageMethod: method,
                }
              ).message;
              return Promise.reject({
                message: String(normalizedError),
              });
            }
          },
          // Private storage.local JSONFile backend methods (used internally by the child
          // ext-storage.js module).
          JSONFileBackend: {
            get(spec) {
              return ExtensionStorage.get(extension.id, spec);
            },
            getKeys() {
              return ExtensionStorage.getKeys(extension.id);
            },
            set(items) {
              return ExtensionStorage.set(extension.id, items);
            },
            remove(keys) {
              return ExtensionStorage.remove(extension.id, keys);
            },
            clear() {
              return ExtensionStorage.clear(extension.id);
            },
          },
          // Private storage.local IDB backend methods (used internally by the child ext-storage.js
          // module).
          IDBBackend: {
            selectBackend() {
              return ExtensionStorageIDB.selectBackend(context);
            },
          },
          onChanged: new EventManager({
            context,
            module: "storage",
            event: "local.onChanged",
            extensionApi: this,
          }).api(),
        },

        session: {
          get QUOTA_BYTES() {
            return extensionStorageSession.QUOTA_BYTES;
          },
          get(items) {
            return extensionStorageSession.get(extension, items);
          },
          getKeys() {
            return extensionStorageSession.getKeys(extension);
          },
          set(items) {
            extensionStorageSession.set(extension, items);
          },
          remove(keys) {
            extensionStorageSession.remove(extension, keys);
          },
          clear() {
            extensionStorageSession.clear(extension);
          },
          getBytesInUse(keys) {
            return extensionStorageSession.getBytesInUse(extension, keys);
          },
          onChanged: new EventManager({
            context,
            module: "storage",
            event: "session.onChanged",
            extensionApi: this,
          }).api(),
        },

        sync: {
          get(spec) {
            enforceNoTemporaryAddon(extension.id);
            recordSyncQuotaTelemetry(extension, context);
            return extensionStorageSync.get(extension, spec, context);
          },
          getKeys() {
            enforceNoTemporaryAddon(extension.id);
            recordSyncQuotaTelemetry(extension, context);
            return extensionStorageSync.getKeys(extension, context);
          },
          set(items) {
            enforceNoTemporaryAddon(extension.id);
            recordSyncQuotaTelemetry(extension, context);
            return extensionStorageSync.set(extension, items, context);
          },
          remove(keys) {
            enforceNoTemporaryAddon(extension.id);
            recordSyncQuotaTelemetry(extension, context);
            return extensionStorageSync.remove(extension, keys, context);
          },
          clear() {
            enforceNoTemporaryAddon(extension.id);
            recordSyncQuotaTelemetry(extension, context);
            return extensionStorageSync.clear(extension, context);
          },
          getBytesInUse(keys) {
            enforceNoTemporaryAddon(extension.id);
            recordSyncQuotaTelemetry(extension, context);
            return extensionStorageSync.getBytesInUse(extension, keys, context);
          },
          onChanged: new EventManager({
            context,
            module: "storage",
            event: "sync.onChanged",
            extensionApi: this,
          }).api(),
        },

        managed: {
          async get(keys) {
            enforceNoTemporaryAddon(extension.id);
            let data = await getManagedStorageManifestData(extension, context);
            return ExtensionStorage._filterProperties(extension.id, data, keys);
          },
          async getKeys() {
            enforceNoTemporaryAddon(extension.id);
            let data = await getManagedStorageManifestData(extension, context);
            return data.keys().toArray();
          },
          // managed storage is currently initialized once.
          onChanged: ignoreEvent(context, "storage.managed.onChanged"),
        },

        onChanged: new EventManager({
          context,
          module: "storage",
          event: "onChanged",
          extensionApi: this,
        }).api(),
      },
    };
  }
};
