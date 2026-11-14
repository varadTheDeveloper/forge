import "./side-effect.js";

export let counter = 0;
export const value = 1;

export function add(a, b) {
  return a + b;
}

export function bump() {
  counter++;
}
