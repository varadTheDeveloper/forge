// Phase 10.4: a message that is queued keeps the program running until it
// is delivered (it is a task), but a started port with nothing queued does
// not: in one program only running code can post to it, so once nothing
// else is pending no message can ever arrive. (Node keeps a port with a
// listener alive until it is closed.) tests/run-runtime-tests.ps1 checks
// that the program ends quickly.

const { port1, port2 } = new MessageChannel();
port2.onmessage = (e) => console.log("delivered: " + e.data);
const idle = new MessageChannel();
idle.port1.onmessage = () => console.log("SHOULD NOT RECEIVE");
idle.port2.start();
port1.postMessage("last");
console.log("idle start");
