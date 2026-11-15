import { shout, marker } from "@forge-test/util";
import { extra } from "greet/extra";  // a package importing itself by name

export default shout("hello from greet");
export const utilMarker = marker;
export const selfExtra = extra;
