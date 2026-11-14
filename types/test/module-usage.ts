// Compile-only checks for import.meta (Phase 9.1). A module, so import.meta
// is allowed.
export {};

const url: string = import.meta.url;
const main: boolean = import.meta.main;
const resolved: string = import.meta.resolve("./lib.js");
// @ts-expect-error  only strings are specifiers
import.meta.resolve(42);
const value: number = await Promise.resolve(1);  // top-level await
void url; void main; void resolved; void value;
