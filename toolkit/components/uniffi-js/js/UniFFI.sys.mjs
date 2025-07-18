/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* globals UniFFIScaffolding */

// This JS module contains shared functionality for the generated UniFFI JS
// code.
//
let lazy = {};

ChromeUtils.defineLazyGetter(lazy, "decoder", () => new TextDecoder());
ChromeUtils.defineLazyGetter(lazy, "encoder", () => new TextEncoder());

// TypeError for UniFFI calls
//
// This extends TypeError to add support for recording a nice description of
// the item that fails the type check. This is especially useful for invalid
// values nested in objects/arrays/maps, etc.
//
// To accomplish this, the FfiConverter.checkType methods of records, arrays,
// maps, etc. catch UniFFITypeError, call `addItemDescriptionPart()` with a
// string representing the child item, then re-raise the exception.  We then
// join all the parts together, in reverse order, to create item description
// strings like `foo.bar[123]["key"]`
export class UniFFITypeError extends TypeError {
  constructor(reason) {
    // our `message` getter isn't invoked in all cases, so we supply a default
    // to the `TypeError` constructor.
    super(reason);
    this.reason = reason;
    this.itemDescriptionParts = [];
  }

  addItemDescriptionPart(part) {
    this.itemDescriptionParts.push(part);
    this.updateMessage();
  }

  itemDescription() {
    const itemDescriptionParts = [...this.itemDescriptionParts];
    itemDescriptionParts.reverse();
    return itemDescriptionParts.join("");
  }

  updateMessage() {
    this.message = `${this.itemDescription()}: ${this.reason}`;
  }
}

// Write/Read data to/from an ArrayBuffer
export class ArrayBufferDataStream {
  constructor(arrayBuffer) {
    this.dataView = new DataView(arrayBuffer);
    this.pos = 0;
  }

  readUint8() {
    let rv = this.dataView.getUint8(this.pos);
    this.pos += 1;
    return rv;
  }

  writeUint8(value) {
    this.dataView.setUint8(this.pos, value);
    this.pos += 1;
  }

  readUint16() {
    let rv = this.dataView.getUint16(this.pos);
    this.pos += 2;
    return rv;
  }

  writeUint16(value) {
    this.dataView.setUint16(this.pos, value);
    this.pos += 2;
  }

  readUint32() {
    let rv = this.dataView.getUint32(this.pos);
    this.pos += 4;
    return rv;
  }

  writeUint32(value) {
    this.dataView.setUint32(this.pos, value);
    this.pos += 4;
  }

  readUint64() {
    let rv = this.dataView.getBigUint64(this.pos);
    this.pos += 8;
    return Number(rv);
  }

  writeUint64(value) {
    this.dataView.setBigUint64(this.pos, BigInt(value));
    this.pos += 8;
  }

  readInt8() {
    let rv = this.dataView.getInt8(this.pos);
    this.pos += 1;
    return rv;
  }

  writeInt8(value) {
    this.dataView.setInt8(this.pos, value);
    this.pos += 1;
  }

  readInt16() {
    let rv = this.dataView.getInt16(this.pos);
    this.pos += 2;
    return rv;
  }

  writeInt16(value) {
    this.dataView.setInt16(this.pos, value);
    this.pos += 2;
  }

  readInt32() {
    let rv = this.dataView.getInt32(this.pos);
    this.pos += 4;
    return rv;
  }

  writeInt32(value) {
    this.dataView.setInt32(this.pos, value);
    this.pos += 4;
  }

  readInt64() {
    let rv = this.dataView.getBigInt64(this.pos);
    this.pos += 8;
    return Number(rv);
  }

  writeInt64(value) {
    this.dataView.setBigInt64(this.pos, BigInt(value));
    this.pos += 8;
  }

  readFloat32() {
    let rv = this.dataView.getFloat32(this.pos);
    this.pos += 4;
    return rv;
  }

  writeFloat32(value) {
    this.dataView.setFloat32(this.pos, value);
    this.pos += 4;
  }

  readFloat64() {
    let rv = this.dataView.getFloat64(this.pos);
    this.pos += 8;
    return rv;
  }

  writeFloat64(value) {
    this.dataView.setFloat64(this.pos, value);
    this.pos += 8;
  }

  writeString(value) {
    // Note: in order to efficiently write this data, we first write the
    // string data, reserving 4 bytes for the size.
    const dest = new Uint8Array(this.dataView.buffer, this.pos + 4);
    const encodeResult = lazy.encoder.encodeInto(value, dest);
    if (encodeResult.read != value.length) {
      throw new UniFFIError(
        "writeString: out of space when writing to ArrayBuffer.  Did the computeSize() method returned the wrong result?"
      );
    }
    const size = encodeResult.written;
    // Next, go back and write the size before the string data
    this.dataView.setUint32(this.pos, size);
    // Finally, advance our position past both the size and string data
    this.pos += size + 4;
  }

  readString() {
    const size = this.readUint32();
    const source = new Uint8Array(this.dataView.buffer, this.pos, size);
    const value = lazy.decoder.decode(source);
    this.pos += size;
    return value;
  }

  readBytes() {
    const size = this.readInt32();
    const bytes = new Uint8Array(this.dataView.buffer, this.pos, size);
    this.pos += size;
    return bytes;
  }

  writeBytes(value) {
    this.writeUint32(value.length);
    value.forEach(elt => {
      this.writeUint8(elt);
    });
  }

  // Reads a pointer from the data stream
  // UniFFI Pointers are **always** 8 bytes long. That is enforced
  // by the C++ and Rust Scaffolding code.
  readPointer(pointerId) {
    const res = UniFFIScaffolding.readPointer(
      pointerId,
      this.dataView.buffer,
      this.pos
    );
    this.pos += 8;
    return res;
  }

  // Writes a pointer into the data stream
  // UniFFI Pointers are **always** 8 bytes long. That is enforced
  // by the C++ and Rust Scaffolding code.
  writePointer(pointerId, value) {
    UniFFIScaffolding.writePointer(
      pointerId,
      value,
      this.dataView.buffer,
      this.pos
    );
    this.pos += 8;
  }
}

// Base class for FFI converters
export class FfiConverter {
  // throw `UniFFITypeError` if a value to be converted has an invalid type
  static checkType(value) {
    if (value === undefined) {
      throw new UniFFITypeError(`undefined`);
    }
    if (value === null) {
      throw new UniFFITypeError(`null`);
    }
  }
}

// Base class for FFI converters that lift/lower by reading/writing to an ArrayBuffer
export class FfiConverterArrayBuffer extends FfiConverter {
  static lift(buf) {
    return this.read(new ArrayBufferDataStream(buf));
  }

  static lower(value) {
    const buf = new ArrayBuffer(this.computeSize(value));
    const dataStream = new ArrayBufferDataStream(buf);
    this.write(dataStream, value);
    return buf;
  }

  /**
   * Computes the size of the value.
   *
   * @param {*} _value
   * @returns {number}
   */
  static computeSize(_value) {
    throw new UniFFIInternalError(
      "computeSize() should be declared in the derived class"
    );
  }

  /**
   * Reads the type from a data stream.
   *
   * @param {ArrayBufferDataStream} _dataStream
   * @returns {any}
   */
  static read(_dataStream) {
    throw new UniFFIInternalError(
      "read() should be declared in the derived class"
    );
  }

  /**
   * Writes the type to a data stream.
   *
   * @param {ArrayBufferDataStream} _dataStream
   * @param {any} _value
   */
  static write(_dataStream, _value) {
    throw new UniFFIInternalError(
      "write() should be declared in the derived class"
    );
  }
}

export class FfiConverterInt8 extends FfiConverter {
  static checkType(value) {
    super.checkType(value);
    if (!Number.isInteger(value)) {
      throw new UniFFITypeError(`${value} is not an integer`);
    }
    if (value < -128 || value > 127) {
      throw new UniFFITypeError(`${value} exceeds the I8 bounds`);
    }
  }
  static computeSize(_value) {
    return 1;
  }
  static lift(value) {
    return value;
  }
  static lower(value) {
    return value;
  }
  static write(dataStream, value) {
    dataStream.writeInt8(value);
  }
  static read(dataStream) {
    return dataStream.readInt8();
  }
}

export class FfiConverterUInt8 extends FfiConverter {
  static checkType(value) {
    super.checkType(value);
    if (!Number.isInteger(value)) {
      throw new UniFFITypeError(`${value} is not an integer`);
    }
    if (value < 0 || value > 256) {
      throw new UniFFITypeError(`${value} exceeds the U8 bounds`);
    }
  }
  static computeSize(_value) {
    return 1;
  }
  static lift(value) {
    return value;
  }
  static lower(value) {
    return value;
  }
  static write(dataStream, value) {
    dataStream.writeUint8(value);
  }
  static read(dataStream) {
    return dataStream.readUint8();
  }
}

export class FfiConverterInt16 extends FfiConverter {
  static checkType(value) {
    super.checkType(value);
    if (!Number.isInteger(value)) {
      throw new UniFFITypeError(`${value} is not an integer`);
    }
    if (value < -32768 || value > 32767) {
      throw new UniFFITypeError(`${value} exceeds the I16 bounds`);
    }
  }
  static computeSize(_value) {
    return 2;
  }
  static lift(value) {
    return value;
  }
  static lower(value) {
    return value;
  }
  static write(dataStream, value) {
    dataStream.writeInt16(value);
  }
  static read(dataStream) {
    return dataStream.readInt16();
  }
}

export class FfiConverterUInt16 extends FfiConverter {
  static checkType(value) {
    super.checkType(value);
    if (!Number.isInteger(value)) {
      throw new UniFFITypeError(`${value} is not an integer`);
    }
    if (value < 0 || value > 65535) {
      throw new UniFFITypeError(`${value} exceeds the U16 bounds`);
    }
  }
  static computeSize(_value) {
    return 2;
  }
  static lift(value) {
    return value;
  }
  static lower(value) {
    return value;
  }
  static write(dataStream, value) {
    dataStream.writeUint16(value);
  }
  static read(dataStream) {
    return dataStream.readUint16();
  }
}

export class FfiConverterInt32 extends FfiConverter {
  static checkType(value) {
    super.checkType(value);
    if (!Number.isInteger(value)) {
      throw new UniFFITypeError(`${value} is not an integer`);
    }
    if (value < -2147483648 || value > 2147483647) {
      throw new UniFFITypeError(`${value} exceeds the I32 bounds`);
    }
  }
  static computeSize(_value) {
    return 4;
  }
  static lift(value) {
    return value;
  }
  static lower(value) {
    return value;
  }
  static write(dataStream, value) {
    dataStream.writeInt32(value);
  }
  static read(dataStream) {
    return dataStream.readInt32();
  }
}

export class FfiConverterUInt32 extends FfiConverter {
  static checkType(value) {
    super.checkType(value);
    if (!Number.isInteger(value)) {
      throw new UniFFITypeError(`${value} is not an integer`);
    }
    if (value < 0 || value > 4294967295) {
      throw new UniFFITypeError(`${value} exceeds the U32 bounds`);
    }
  }
  static computeSize(_value) {
    return 4;
  }
  static lift(value) {
    return value;
  }
  static lower(value) {
    return value;
  }
  static write(dataStream, value) {
    dataStream.writeUint32(value);
  }
  static read(dataStream) {
    return dataStream.readUint32();
  }
}

export class FfiConverterInt64 extends FfiConverter {
  static checkType(value) {
    super.checkType(value);
    if (!Number.isSafeInteger(value)) {
      throw new UniFFITypeError(`${value} exceeds the safe integer bounds`);
    }
  }
  static computeSize(_value) {
    return 8;
  }
  static lift(value) {
    return value;
  }
  static lower(value) {
    return value;
  }
  static write(dataStream, value) {
    dataStream.writeInt64(value);
  }
  static read(dataStream) {
    return dataStream.readInt64();
  }
}

export class FfiConverterUInt64 extends FfiConverter {
  static checkType(value) {
    super.checkType(value);
    if (!Number.isSafeInteger(value)) {
      throw new UniFFITypeError(`${value} exceeds the safe integer bounds`);
    }
    if (value < 0) {
      throw new UniFFITypeError(`${value} exceeds the U64 bounds`);
    }
  }
  static computeSize(_value) {
    return 8;
  }
  static lift(value) {
    return value;
  }
  static lower(value) {
    return value;
  }
  static write(dataStream, value) {
    dataStream.writeUint64(value);
  }
  static read(dataStream) {
    return dataStream.readUint64();
  }
}

export class FfiConverterFloat32 extends FfiConverter {
  static computeSize(_value) {
    return 4;
  }
  static lift(value) {
    return value;
  }
  static lower(value) {
    return value;
  }
  static write(dataStream, value) {
    dataStream.writeFloat32(value);
  }
  static read(dataStream) {
    return dataStream.readFloat32();
  }
}
// Export the FFIConverter object to make external types work.
export class FfiConverterFloat64 extends FfiConverter {
  static computeSize(_value) {
    return 8;
  }
  static lift(value) {
    return value;
  }
  static lower(value) {
    return value;
  }
  static write(dataStream, value) {
    dataStream.writeFloat64(value);
  }
  static read(dataStream) {
    return dataStream.readFloat64();
  }
}

export class FfiConverterBoolean extends FfiConverter {
  static computeSize(_value) {
    return 1;
  }
  static lift(value) {
    return value == 1;
  }
  static lower(value) {
    if (value) {
      return 1;
    }
    return 0;
  }
  static write(dataStream, value) {
    dataStream.writeUint8(this.lower(value));
  }
  static read(dataStream) {
    return this.lift(dataStream.readUint8());
  }
}

// Export the FFIConverter object to make external types work.
export class FfiConverterString extends FfiConverter {
  static checkType(value) {
    super.checkType(value);
    if (typeof value !== "string") {
      throw new UniFFITypeError(`${value} is not a string`);
    }
  }

  static lift(buf) {
    const utf8Arr = new Uint8Array(buf);
    return lazy.decoder.decode(utf8Arr);
  }
  static lower(value) {
    return lazy.encoder.encode(value).buffer;
  }

  static write(dataStream, value) {
    dataStream.writeString(value);
  }

  static read(dataStream) {
    return dataStream.readString();
  }

  static computeSize(value) {
    return 4 + lazy.encoder.encode(value).length;
  }
}

export class FfiConverterBytes extends FfiConverterArrayBuffer {
  static read(dataStream) {
    return dataStream.readBytes();
  }

  static write(dataStream, value) {
    dataStream.writeBytes(value);
  }

  static computeSize(value) {
    // The size of the length + 1 byte / item
    return 4 + value.length;
  }

  static checkType(value) {
    if (!(value instanceof Uint8Array)) {
      throw new UniFFITypeError(`${value} is not an Uint8Array`);
    }
  }
}

export function handleRustResult(result, liftCallback, liftErrCallback) {
  switch (result.code) {
    case "success":
      return liftCallback(result.data);

    case "error":
      throw liftErrCallback(result.data);

    case "internal-error":
      if (result.data) {
        throw new UniFFIInternalError(FfiConverterString.lift(result.data));
      } else {
        throw new UniFFIInternalError("Unknown error");
      }

    default:
      throw new UniFFIError(`Unexpected status code: ${result.code}`);
  }
}

export class UniFFIError {
  constructor(message) {
    this.message = message;
  }

  toString() {
    return `UniFFIError: ${this.message}`;
  }
}

export class UniFFIInternalError extends UniFFIError {}

// Symbols that are used to ensure that Object constructors
// can only be used with a proper UniFFI pointer
export const uniffiObjectPtr = Symbol("uniffiObjectPtr");
export const constructUniffiObject = Symbol("constructUniffiObject");

/**
 * Handler for a single UniFFI CallbackInterface
 *
 * This class stores objects that implement a callback interface in a handle
 * map, allowing them to be referenced by the Rust code using an integer
 * handle.
 *
 * While the callback object is stored in the map, it allows the Rust code to
 * call methods on the object using the callback object handle, a method id,
 * and an ArrayBuffer packed with the method arguments.
 *
 * When the Rust code drops its reference, it sends a call with the methodId=0,
 * which causes callback object to be removed from the map.
 */
export class UniFFICallbackHandler {
  #name;
  #interfaceId;
  #handleCounter;
  #handleMap;
  #methodHandlers;
  #allowNewCallbacks;

  /**
   * Create a UniFFICallbackHandler
   *
   * @param {string} name - Human-friendly name for this callback interface
   * @param {int} interfaceId - Interface ID for this CallbackInterface.
   * @param {UniFFICallbackMethodHandler[]} methodHandlers -- UniFFICallbackHandler for each method, in the same order as the UDL file
   */
  constructor(name, interfaceId, methodHandlers) {
    this.#name = name;
    this.#interfaceId = interfaceId;
    this.#handleCounter = 0;
    this.#handleMap = new Map();
    this.#methodHandlers = methodHandlers;
    this.#allowNewCallbacks = true;

    UniFFIScaffolding.registerCallbackHandler(this.#interfaceId, this);
    Services.obs.addObserver(this, "xpcom-shutdown");
  }

  /**
   * Store a callback object in the handle map and return the handle
   *
   * @param {obj} callbackObj - Object that implements the callback interface
   * @returns {int} - Handle for this callback object, this is what gets passed back to Rust.
   */
  storeCallbackObj(callbackObj) {
    if (!this.#allowNewCallbacks) {
      throw new UniFFIError(`No new callbacks allowed for ${this.#name}`);
    }
    // Increment first.  This way handles start at `1` and we can use `0` to represent a NULL
    // handle.
    this.#handleCounter += 1;
    const handle = this.#handleCounter;
    this.#handleMap.set(
      handle,
      new UniFFICallbackHandleMapEntry(
        callbackObj,
        Components.stack.caller.formattedStack.trim()
      )
    );
    return handle;
  }

  /**
   * Get a previously stored callback object
   *
   * @param {int} handle - Callback object handle, returned from `storeCallbackObj()`
   * @returns {obj} - Callback object
   */
  getCallbackObj(handle) {
    const callbackObj = this.#handleMap.get(handle).callbackObj;
    if (callbackObj === undefined) {
      throw new UniFFIError(
        `${this.#name}: invalid callback handle id: ${handle}`
      );
    }
    return callbackObj;
  }

  /**
   * Get a UniFFICallbackMethodHandler
   *
   * @param {int} methodId - index of the method
   * @returns {UniFFICallbackMethodHandler}
   */
  getMethodHandler(methodId) {
    const methodHandler = this.#methodHandlers[methodId];
    if (methodHandler === undefined) {
      throw new UniFFIError(`${this.#name}: invalid method id: ${methodId}`);
    }
    return methodHandler;
  }

  /**
   * Set if new callbacks are allowed for this handler
   *
   * This is called with false during shutdown to ensure the callback maps don't
   * prevent JS objects from being GCed.
   */
  setAllowNewCallbacks(allow) {
    this.#allowNewCallbacks = allow;
  }

  /**
   * Check if there are any registered callbacks in the handle map
   *
   * This is used in the unit tests
   */
  hasRegisteredCallbacks() {
    return this.#handleMap.size > 0;
  }
  /**
   * Check that no callbacks are currently registered
   *
   * If there are callbacks registered a UniFFIError will be thrown.  This is
   * called during shutdown to generate an alert if there are leaked callback
   * interfaces.
   */
  assertNoRegisteredCallbacks() {
    if (this.#handleMap.size > 0) {
      const entry = this.#handleMap.values().next().value;
      throw new UniFFIError(
        `UniFFI interface ${this.#name} has ${this.#handleMap.size} registered callbacks at xpcom-shutdown. This likely indicates a UniFFI callback leak.\nStack trace for the first leaked callback:\n${entry.stackTrace}.`
      );
    }
  }

  /**
   * Invoke a method on a stored callback object
   *
   * @param {int} handle - Object handle
   * @param {int} methodId - Method index (0-based)
   * @param {UniFFIScaffoldingValue[]} args - Arguments to pass to the method
   */
  call(handle, methodId, ...args) {
    try {
      const callbackObj = this.getCallbackObj(handle);
      const methodHandler = this.getMethodHandler(methodId);
      methodHandler.call(callbackObj, args);
    } catch (e) {
      console.error(`internal error invoking callback: ${e}`);
    }
  }

  /**
   * Invoke a method on a stored callback object
   *
   * @param {int} handle - Object handle
   * @param {int} methodId - Method index (0-based)
   * @param {UniFFIScaffoldingValue[]} args - Arguments to pass to the method
   */
  async callAsync(handle, methodId, ...args) {
    const callbackObj = this.getCallbackObj(handle);
    const methodHandler = this.getMethodHandler(methodId);
    try {
      const returnValue = await methodHandler.call(callbackObj, args);
      return methodHandler.lowerReturn(returnValue);
    } catch (e) {
      return methodHandler.lowerError(e);
    }
  }

  /**
   * Destroy a stored callback object
   *
   * @param {int} handle - Object handle
   */
  destroy(handle) {
    this.#handleMap.delete(handle);
  }

  /**
   * xpcom-shutdown observer method
   *
   * This handles:
   *  - Deregistering ourselves as the UniFFI callback handler
   *  - Checks for any leftover stored callbacks which indicate memory leaks
   */
  observe(aSubject, aTopic, _aData) {
    if (aTopic == "xpcom-shutdown") {
      try {
        this.setAllowNewCallbacks(false);
        this.assertNoRegisteredCallbacks();
        UniFFIScaffolding.deregisterCallbackHandler(this.#interfaceId);
      } catch (ex) {
        console.error(
          `UniFFI Callback interface error during xpcom-shutdown: ${ex}`
        );
        Cc["@mozilla.org/xpcom/debug;1"]
          .getService(Ci.nsIDebug2)
          .abort(ex.filename, ex.lineNumber);
      }
    }
  }
}

/**
 * Handles calling a single method for a callback interface
 */
export class UniFFICallbackMethodHandler {
  #name;
  #argsConverters;
  #returnConverter;
  #errorConverter;

  /**
     * Create a UniFFICallbackMethodHandler

     * @param {string} name -- Name of the method to call on the callback object
     * @param {FfiConverter[]} argsConverters - FfiConverter for each argument type
     */
  constructor(name, argsConverters, returnConverter, errorConverter) {
    this.#name = name;
    this.#argsConverters = argsConverters;
    this.#returnConverter = returnConverter;
    this.#errorConverter = errorConverter;
  }

  call(callbackObj, args) {
    const convertedArgs = this.#argsConverters.map((converter, i) =>
      converter.lift(args[i])
    );
    return callbackObj[this.#name](...convertedArgs);
  }

  lowerReturn(returnValue) {
    return {
      code: "success",
      data: this.#returnConverter(returnValue),
    };
  }

  lowerError(error) {
    return {
      code: "error",
      data: this.#errorConverter(error),
    };
  }

  toString() {
    return `CallbackMethodHandler(${this.#name})`;
  }
}

/**
 * UniFFICallbackHandler.handleMap entry
 *
 * @property callbackObj - Callback object, this must implement the callback interface.
 * @property {string} stackTrace - Stack trace from when the callback object was registered.  This is used to proved extra context when debugging leaked callback objects.
 */
class UniFFICallbackHandleMapEntry {
  constructor(callbackObj, stackTrace) {
    this.callbackObj = callbackObj;
    this.stackTrace = stackTrace;
  }
}
