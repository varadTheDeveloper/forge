// Forge's web-standard APIs, the WebIDL layer (Phase 10, PHASE10_DESIGN.md,
// decision D27).
//
// runtime/WebApis.cpp evaluates this file once in every realm (the
// program's and each package compartment's), right after the engine's own
// classes and before any other script, and calls the function with:
//
//   natives    Forge's C++ helpers for this file (byte work: UTF-8, base64,
//              the other decoders; errors with clean stacks; calls on objects
//              of another compartment). The object is never reachable from
//              any other script.
//   global     the realm's global object.
//   internals  an object only this file and C++ hold (a reserved slot of the
//              global): the DOMException records, which console output reads
//              without running script, and the members table used to
//              forward calls across compartments.
//
// Rules for this file (it runs where user code can later change any
// built-in object):
//   1. Everything a function needs when it is CALLED is captured here, at
//      set-up, before user code exists: no `Foo.prototype.bar(...)`, no
//      array destructuring or spread or for-of (they use the array
//      iterator), no method call on a value user code made.
//   2. State lives in private fields or in WeakMaps captured here.
//   3. WebIDL conversions (ToString, dictionary members) do run user code,
//      exactly where the standards say they do, and nowhere else.
//   4. Every global this file defines is listed in kWebApiGlobalNames in
//      WebApis.cpp; DefineWebApis checks that the two agree.
//
// tools/gen-webapis.py turns this file into runtime/WebApis.js.inc; CI
// checks that the two match.

(function (natives, global, internals) {
  'use strict';

  // ------------------------------------------------------------------
  // Primordials
  // ------------------------------------------------------------------

  const uncurryThis = Function.prototype.bind.bind(Function.prototype.call);
  const ObjectDefineProperty = Object.defineProperty;
  const ObjectFreeze = Object.freeze;
  const ObjectGetOwnPropertyDescriptor = Object.getOwnPropertyDescriptor;
  const ObjectGetOwnPropertyNames = Object.getOwnPropertyNames;
  const ObjectGetPrototypeOf = Object.getPrototypeOf;
  const ObjectSetPrototypeOf = Object.setPrototypeOf;
  const ErrorPrototype = Error.prototype;
  const WeakMapCtor = WeakMap;
  const WeakMapGet = uncurryThis(WeakMap.prototype.get);
  const WeakMapSet = uncurryThis(WeakMap.prototype.set);
  const StringPrototypeSlice = uncurryThis(String.prototype.slice);
  const SymbolToStringTag = Symbol.toStringTag;

  const {
    makeError,
    forward,
    encodingForLabel,
    encodingName,
    isBufferSource,
    decode: nativeDecode,
    utf8Encode,
    utf8EncodeInto,
    atob: nativeAtob,
    btoa: nativeBtoa,
  } = natives;

  // makeError kinds. The stack of an error made by makeError starts at the
  // first frame outside Forge's own scripts, so errors point at user code.
  const kError = 0;
  const kTypeError = 1;
  const kRangeError = 2;

  function typeError(message) {
    return makeError(kTypeError, message, undefined);
  }

  function rangeError(message) {
    return makeError(kRangeError, message, undefined);
  }

  // ------------------------------------------------------------------
  // WebIDL helpers
  // ------------------------------------------------------------------

  // "Interface.member" -> the method or getter. A method of one
  // compartment called on an object of another (which reaches it as a
  // cross-compartment wrapper) fails its brand check; `forward` then calls
  // the object's own compartment's member on the object itself, the way
  // the engine's built-in methods handle wrappers.
  const members = { __proto__: null };
  internals.members = members;

  function notBranded(iface, member, self, args) {
    const result = { __proto__: null, value: undefined };

    if (forward(`${iface}.${member}`, self, args, result)) {
      return result.value;
    }

    throw typeError(`'${member}' called on an object that does not implement interface ${iface}.`);
  }

  function requireArguments(count, required, where) {
    if (count < required) {
      throw typeError(`${where}: ${required} argument${required === 1 ? '' : 's'} required, ` +
                      `but only ${count} present.`);
    }
  }

  // WebIDL dictionary conversion: undefined and null are an empty
  // dictionary; any other non-object is a TypeError. Members are then read
  // with ordinary [[Get]], in lexicographic order, by the caller.
  function toDictionary(value, where) {
    if (value === undefined || value === null) {
      return undefined;
    }

    const type = typeof value;

    if (type !== 'object' && type !== 'function') {
      throw typeError(`${where}: the options argument is not an object.`);
    }

    return value;
  }

  // Interface objects: members enumerable (WebIDL), a @@toStringTag, the
  // constants, the global property (writable, configurable, not
  // enumerable).
  function defineInterface(ctor, name, constants) {
    const proto = ctor.prototype;
    const keys = ObjectGetOwnPropertyNames(proto);

    for (let i = 0; i < keys.length; i++) {
      const key = keys[i];

      if (key === 'constructor') {
        continue;
      }

      const desc = ObjectGetOwnPropertyDescriptor(proto, key);
      desc.enumerable = true;
      ObjectDefineProperty(proto, key, desc);
      members[`${name}.${key}`] = desc.get !== undefined ? desc.get : desc.value;
    }

    ObjectDefineProperty(proto, SymbolToStringTag, {
      value: name, writable: false, enumerable: false, configurable: true,
    });

    for (let i = 0; constants !== undefined && i < constants.length; i++) {
      const constant = { value: i + 1, writable: false, enumerable: true, configurable: false };
      ObjectDefineProperty(ctor, constants[i], constant);
      ObjectDefineProperty(proto, constants[i], constant);
    }

    ObjectDefineProperty(global, name, {
      value: ctor, writable: true, enumerable: false, configurable: true,
    });
  }

  // ------------------------------------------------------------------
  // DOMException (WebIDL)
  // ------------------------------------------------------------------

  // Error object -> frozen { name, message, code }. Read by the getters,
  // and by console output in C++ (WebApis.cpp, GetDomExceptionFields).
  const domExceptions = new WeakMapCtor();
  internals.domExceptions = domExceptions;

  // The WebIDL "error names" table, for names with a legacy code.
  const legacyCodes = {
    __proto__: null,
    IndexSizeError: 1,
    HierarchyRequestError: 3,
    WrongDocumentError: 4,
    InvalidCharacterError: 5,
    NoModificationAllowedError: 7,
    NotFoundError: 8,
    NotSupportedError: 9,
    InUseAttributeError: 10,
    InvalidStateError: 11,
    SyntaxError: 12,
    InvalidModificationError: 13,
    NamespaceError: 14,
    InvalidAccessError: 15,
    TypeMismatchError: 17,
    SecurityError: 18,
    NetworkError: 19,
    AbortError: 20,
    URLMismatchError: 21,
    QuotaExceededError: 22,
    TimeoutError: 23,
    InvalidNodeTypeError: 24,
    DataCloneError: 25,
  };

  // The legacy constants, valued 1 to 25 in this order.
  const legacyConstants = [
    'INDEX_SIZE_ERR', 'DOMSTRING_SIZE_ERR', 'HIERARCHY_REQUEST_ERR', 'WRONG_DOCUMENT_ERR',
    'INVALID_CHARACTER_ERR', 'NO_DATA_ALLOWED_ERR', 'NO_MODIFICATION_ALLOWED_ERR',
    'NOT_FOUND_ERR', 'NOT_SUPPORTED_ERR', 'INUSE_ATTRIBUTE_ERR', 'INVALID_STATE_ERR',
    'SYNTAX_ERR', 'INVALID_MODIFICATION_ERR', 'NAMESPACE_ERR', 'INVALID_ACCESS_ERR',
    'VALIDATION_ERR', 'TYPE_MISMATCH_ERR', 'SECURITY_ERR', 'NETWORK_ERR', 'ABORT_ERR',
    'URL_MISMATCH_ERR', 'QUOTA_EXCEEDED_ERR', 'TIMEOUT_ERR', 'INVALID_NODE_TYPE_ERR',
    'DATA_CLONE_ERR',
  ];

  // A DOMException is a real Error object (so it has a stack, and
  // Error.prototype.toString works), whose prototype is
  // DOMException.prototype and whose name, message and code are internal:
  // none of them is an own property.
  class DOMException {
    constructor(message = undefined, name = undefined) {
      message = message === undefined ? '' : `${message}`;
      name = name === undefined ? 'Error' : `${name}`;
      // `this` already has new.target's prototype (or this realm's
      // DOMException.prototype); the Error takes it over.
      const error = makeError(kError, undefined, ObjectGetPrototypeOf(this));
      const code = legacyCodes[name];
      WeakMapSet(domExceptions, error, ObjectFreeze({
        __proto__: null, name, message, code: code === undefined ? 0 : code,
      }));
      return error;
    }

    get name() {
      const record = WeakMapGet(domExceptions, this);
      return record !== undefined ? record.name : notBranded('DOMException', 'name', this, arguments);
    }

    get message() {
      const record = WeakMapGet(domExceptions, this);
      return record !== undefined ? record.message
                                  : notBranded('DOMException', 'message', this, arguments);
    }

    get code() {
      const record = WeakMapGet(domExceptions, this);
      return record !== undefined ? record.code : notBranded('DOMException', 'code', this, arguments);
    }
  }

  // WebIDL: DOMException.prototype inherits from Error.prototype (so
  // toString and instanceof Error work), while DOMException itself inherits
  // from Function.prototype, which `class ... extends Error` would not give.
  ObjectSetPrototypeOf(DOMException.prototype, ErrorPrototype);
  defineInterface(DOMException, 'DOMException', legacyConstants);

  // ------------------------------------------------------------------
  // TextEncoder (Encoding Standard)
  // ------------------------------------------------------------------

  class TextEncoder {
    #brand = true;

    static #is(value) {
      return value !== null && (typeof value === 'object' || typeof value === 'function') &&
             #brand in value;
    }

    get encoding() {
      if (!TextEncoder.#is(this)) {
        return notBranded('TextEncoder', 'encoding', this, arguments);
      }

      return 'utf-8';
    }

    encode(input = '') {
      if (!TextEncoder.#is(this)) {
        return notBranded('TextEncoder', 'encode', this, arguments);
      }

      // USVString: lone surrogates become U+FFFD in utf8Encode.
      return utf8Encode(`${input}`);
    }

    encodeInto(source, destination) {
      if (!TextEncoder.#is(this)) {
        return notBranded('TextEncoder', 'encodeInto', this, arguments);
      }

      requireArguments(arguments.length, 2, 'TextEncoder.encodeInto');
      const string = `${source}`;
      // Throws a TypeError unless `destination` is a Uint8Array.
      return utf8EncodeInto(string, destination);
    }
  }

  defineInterface(TextEncoder, 'TextEncoder', undefined);

  // ------------------------------------------------------------------
  // TextDecoder (Encoding Standard)
  // ------------------------------------------------------------------

  // encodingForLabel results below zero.
  const kLabelUnknown = -1;
  const kLabelReplacement = -2;

  class TextDecoder {
    #encoding;
    #name;
    #fatal;
    #ignoreBOM;
    #stripBOM;  // UTF-8 or UTF-16, and ignoreBOM is false
    #doNotFlush = false;
    #bomSeen = false;
    #pending = undefined;  // bytes of an incomplete sequence (Uint8Array)

    static #is(value) {
      return value !== null && (typeof value === 'object' || typeof value === 'function') &&
             #encoding in value;
    }

    constructor(label = undefined, options = undefined) {
      label = label === undefined ? 'utf-8' : `${label}`;
      const dictionary = toDictionary(options, 'TextDecoder constructor');
      const fatal = dictionary === undefined ? false : !!dictionary.fatal;
      const ignoreBOM = dictionary === undefined ? false : !!dictionary.ignoreBOM;
      const encoding = encodingForLabel(label);

      if (encoding < 0) {
        throw rangeError(
          encoding === kLabelUnknown || encoding === kLabelReplacement
            ? `TextDecoder constructor: '${label}' is not a supported encoding label.`
            : `TextDecoder constructor: Forge does not decode the encoding of '${label}' yet ` +
              `(see WinterTC.md).`);
      }

      const name = encodingName(encoding);
      this.#encoding = encoding;
      this.#name = name;
      this.#fatal = fatal;
      this.#ignoreBOM = ignoreBOM;
      this.#stripBOM = !ignoreBOM && (name === 'utf-8' || name === 'utf-16le' || name === 'utf-16be');
    }

    get encoding() {
      if (!TextDecoder.#is(this)) {
        return notBranded('TextDecoder', 'encoding', this, arguments);
      }

      return this.#name;
    }

    get fatal() {
      if (!TextDecoder.#is(this)) {
        return notBranded('TextDecoder', 'fatal', this, arguments);
      }

      return this.#fatal;
    }

    get ignoreBOM() {
      if (!TextDecoder.#is(this)) {
        return notBranded('TextDecoder', 'ignoreBOM', this, arguments);
      }

      return this.#ignoreBOM;
    }

    decode(input = undefined, options = undefined) {
      if (!TextDecoder.#is(this)) {
        return notBranded('TextDecoder', 'decode', this, arguments);
      }

      if (input !== undefined && !isBufferSource(input)) {
        throw typeError('TextDecoder.decode: the input is not an ArrayBuffer or an ' +
                        'ArrayBufferView.');
      }

      const dictionary = toDictionary(options, 'TextDecoder.decode');
      const stream = dictionary === undefined ? false : !!dictionary.stream;

      if (!this.#doNotFlush) {
        this.#pending = undefined;
        this.#bomSeen = false;
      }

      this.#doNotFlush = stream;
      const result = nativeDecode(this.#encoding, this.#pending, input, this.#fatal, !stream);

      if (result === null) {
        // A fatal error ends the stream: the next call starts afresh.
        this.#pending = undefined;
        this.#doNotFlush = false;
        this.#bomSeen = false;
        throw typeError(`TextDecoder.decode: the data is not valid ${this.#name}.`);
      }

      this.#pending = result[1];
      let text = result[0];

      if (this.#stripBOM && !this.#bomSeen && text.length > 0) {
        this.#bomSeen = true;

        if (text[0] === '\uFEFF') {
          text = StringPrototypeSlice(text, 1);
        }
      }

      return text;
    }
  }

  defineInterface(TextDecoder, 'TextDecoder', undefined);

  // ------------------------------------------------------------------
  // atob and btoa (HTML Standard)
  // ------------------------------------------------------------------

  // Methods, so they are not constructors and have no prototype property.
  const operations = {
    atob(data) {
      requireArguments(arguments.length, 1, 'atob');
      const result = nativeAtob(`${data}`);

      if (result === null) {
        throw new DOMException('atob: the string is not correctly encoded base64.',
                               'InvalidCharacterError');
      }

      return result;
    },

    btoa(data) {
      requireArguments(arguments.length, 1, 'btoa');
      const result = nativeBtoa(`${data}`);

      if (result === null) {
        throw new DOMException('btoa: the string contains a character outside of Latin-1.',
                               'InvalidCharacterError');
      }

      return result;
    },
  };

  ObjectDefineProperty(global, 'atob', {
    value: operations.atob, writable: true, enumerable: true, configurable: true,
  });
  ObjectDefineProperty(global, 'btoa', {
    value: operations.btoa, writable: true, enumerable: true, configurable: true,
  });
})
