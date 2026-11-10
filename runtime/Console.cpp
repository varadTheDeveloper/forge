#include "runtime/Console.h"

#include <stdio.h>

#include <chrono>
#include <utility>

#include "js/CallArgs.h"
#include "js/Conversions.h"
#include "js/PropertyAndElement.h"
#include "js/PropertySpec.h"
#include "js/Stack.h"
#include "js/String.h"
#include "jsapi.h"

#include "forge-core/String.h"
#include "forge-core/memory/Vector.h"
#include "forge-core/platform/Stdio.h"
#include "runtime/Inspect.h"

namespace forge::runtime
{

namespace
{

using forge::core::Size;
using forge::core::String;
using forge::core::StringView;
using forge::core::platform::StdStream;

struct LabelEntry
{
    String label;
    double value = 0;  // a count, or a start time in milliseconds
};

/// Process-wide console state. Forge has one JS thread, and console calls
/// only happen on it.
struct ConsoleState
{
    forge::core::Vector<LabelEntry> counts;
    forge::core::Vector<LabelEntry> timers;
    unsigned groupIndent = 0;  // spaces
};

ConsoleState g_console;

double NowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

/// Writes `text` and a newline, with every line indented by `indent`
/// spaces. Write errors (a closed pipe, for example) are ignored, as the
/// console standard requires: logging never throws.
void WriteLine(
    StdStream stream,
    StringView text,
    unsigned indent)
{
    String line;
    bool ok = true;

    auto add = [&](StringView piece) {
        if (ok && line.Append(piece).HasError())
        {
            ok = false;
        }
    };

    auto addIndent = [&]() {
        for (unsigned i = 0; i < indent; ++i)
        {
            add(" ");
        }
    };

    addIndent();
    Size start = 0;

    for (Size i = 0; i < text.Size(); ++i)
    {
        if (text.Data()[i] == '\n')
        {
            add(StringView(text.Data() + start, i + 1 - start));
            addIndent();
            start = i + 1;
        }
    }

    add(StringView(text.Data() + start, text.Size() - start));
    add("\n");

    if (ok)
    {
        forge::core::platform::WriteStd(stream, line.Data(), line.Size()).Ignore();
        return;
    }

    // Out of memory while indenting: write the text as it is.
    forge::core::platform::WriteStd(stream, text.Data(), text.Size()).Ignore();
    forge::core::platform::WriteStd(stream, "\n", 1).Ignore();
}

bool Log(
    JSContext* cx,
    const JS::CallArgs& args,
    unsigned first,
    StdStream stream,
    StringView prefix = StringView())
{
    String text;

    if (!prefix.Empty() && text.Append(prefix).HasError())
    {
        JS_ReportOutOfMemory(cx);
        return false;
    }

    if (!AppendFormattedArgs(cx, args, first, text))
    {
        if (!JS_IsExceptionPending(cx))
        {
            JS_ReportOutOfMemory(cx);
        }

        return false;
    }

    WriteLine(stream, text.View(), g_console.groupIndent);
    return true;
}

/// The label argument of count/time/...: "default" when missing or
/// undefined, otherwise the value formatted without running script.
bool GetLabel(
    JSContext* cx,
    const JS::CallArgs& args,
    String& label)
{
    if (!args.hasDefined(0))
    {
        if (label.Append("default").HasError())
        {
            JS_ReportOutOfMemory(cx);
            return false;
        }

        return true;
    }

    InspectOptions options;
    options.depth = 0;

    if (!AppendInspected(cx, args[0], label, options))
    {
        if (!JS_IsExceptionPending(cx))
        {
            JS_ReportOutOfMemory(cx);
        }

        return false;
    }

    return true;
}

Size FindLabel(
    forge::core::Vector<LabelEntry>& entries,
    StringView label)
{
    for (Size i = 0; i < entries.Size(); ++i)
    {
        if (entries[i].label.View() == label)
        {
            return i;
        }
    }

    return static_cast<Size>(-1);
}

void RemoveLabel(
    forge::core::Vector<LabelEntry>& entries,
    Size index)
{
    if (index + 1 < entries.Size())
    {
        entries[index].label.Swap(entries[entries.Size() - 1].label);
        entries[index].value = entries[entries.Size() - 1].value;
    }

    entries.PopBack();
}

/// "<before>'<label>'<after>" to standard error.
void WarnAboutLabel(
    const char* before,
    StringView label,
    const char* after)
{
    String text;

    if (text.Append(before).HasError() || text.Append(label).HasError() ||
        text.Append(after).HasError())
    {
        return;
    }

    WriteLine(StdStream::Err, text.View(), g_console.groupIndent);
}

bool ConsoleLog(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    args.rval().setUndefined();
    return Log(cx, args, 0, StdStream::Out);
}

bool ConsoleError(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    args.rval().setUndefined();
    return Log(cx, args, 0, StdStream::Err);
}

bool ConsoleDir(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    args.rval().setUndefined();

    InspectOptions options;
    options.rawTopLevelStrings = false;
    String text;

    if (!AppendInspected(cx, args.get(0), text, options))
    {
        if (!JS_IsExceptionPending(cx))
        {
            JS_ReportOutOfMemory(cx);
        }

        return false;
    }

    WriteLine(StdStream::Out, text.View(), g_console.groupIndent);
    return true;
}

bool ConsoleAssert(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    args.rval().setUndefined();

    if (JS::ToBoolean(args.get(0)))
    {
        return true;
    }

    if (args.length() <= 1)
    {
        WriteLine(StdStream::Err, "Assertion failed", g_console.groupIndent);
        return true;
    }

    return Log(cx, args, 1, StdStream::Err,
               args[1].isString() ? StringView("Assertion failed: ")
                                  : StringView("Assertion failed "));
}

bool ConsoleTrace(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    args.rval().setUndefined();

    String text;

    if (text.Append(args.length() > 0 ? "Trace: " : "Trace").HasError() ||
        !AppendFormattedArgs(cx, args, 0, text))
    {
        if (!JS_IsExceptionPending(cx))
        {
            JS_ReportOutOfMemory(cx);
        }

        return false;
    }

    JS::RootedObject stack(cx);
    JS::RootedString frames(cx);

    if (!JS::CaptureCurrentStack(cx, &stack) ||
        !JS::BuildStackString(cx, nullptr, stack, &frames, 4))
    {
        return false;
    }

    if (frames)
    {
        JS::UniqueChars utf8 = JS_EncodeStringToUTF8(cx, frames);

        if (!utf8)
        {
            return false;
        }

        StringView view(utf8.get());

        while (!view.Empty() && view.Data()[view.Size() - 1] == '\n')
        {
            view = StringView(view.Data(), view.Size() - 1);
        }

        if (!view.Empty() && (text.Append("\n").HasError() || text.Append(view).HasError()))
        {
            JS_ReportOutOfMemory(cx);
            return false;
        }
    }

    WriteLine(StdStream::Err, text.View(), g_console.groupIndent);
    return true;
}

bool ConsoleCount(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    args.rval().setUndefined();

    String label;

    if (!GetLabel(cx, args, label))
    {
        return false;
    }

    Size index = FindLabel(g_console.counts, label.View());

    if (index == static_cast<Size>(-1))
    {
        LabelEntry entry;
        entry.label.Swap(label);

        if (g_console.counts.PushBack(std::move(entry)).HasError())
        {
            JS_ReportOutOfMemory(cx);
            return false;
        }

        index = g_console.counts.Size() - 1;
    }

    LabelEntry& entry = g_console.counts[index];
    entry.value += 1;

    char number[32];
    snprintf(number, sizeof(number), ": %.0f", entry.value);
    String text;

    if (text.Append(entry.label.View()).HasError() || text.Append(number).HasError())
    {
        JS_ReportOutOfMemory(cx);
        return false;
    }

    WriteLine(StdStream::Out, text.View(), g_console.groupIndent);
    return true;
}

bool ConsoleCountReset(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    args.rval().setUndefined();

    String label;

    if (!GetLabel(cx, args, label))
    {
        return false;
    }

    const Size index = FindLabel(g_console.counts, label.View());

    if (index == static_cast<Size>(-1))
    {
        WarnAboutLabel("Count for '", label.View(), "' does not exist");
        return true;
    }

    g_console.counts[index].value = 0;
    return true;
}

bool ConsoleTime(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    args.rval().setUndefined();

    String label;

    if (!GetLabel(cx, args, label))
    {
        return false;
    }

    if (FindLabel(g_console.timers, label.View()) != static_cast<Size>(-1))
    {
        WarnAboutLabel("Timer '", label.View(), "' already exists");
        return true;
    }

    LabelEntry entry;
    entry.label.Swap(label);
    entry.value = NowMs();

    if (g_console.timers.PushBack(std::move(entry)).HasError())
    {
        JS_ReportOutOfMemory(cx);
        return false;
    }

    return true;
}

/// Shared by timeLog and timeEnd.
bool ReportTimer(JSContext* cx, const JS::CallArgs& args, bool end)
{
    String label;

    if (!GetLabel(cx, args, label))
    {
        return false;
    }

    const Size index = FindLabel(g_console.timers, label.View());

    if (index == static_cast<Size>(-1))
    {
        WarnAboutLabel("Timer '", label.View(), "' does not exist");
        return true;
    }

    char elapsed[48];
    snprintf(elapsed, sizeof(elapsed), ": %.3fms", NowMs() - g_console.timers[index].value);
    String text;

    if (text.Append(label.View()).HasError() || text.Append(elapsed).HasError())
    {
        JS_ReportOutOfMemory(cx);
        return false;
    }

    if (!end && args.length() > 1)
    {
        if (text.Append(" ").HasError() || !AppendFormattedArgs(cx, args, 1, text))
        {
            if (!JS_IsExceptionPending(cx))
            {
                JS_ReportOutOfMemory(cx);
            }

            return false;
        }
    }

    if (end)
    {
        RemoveLabel(g_console.timers, index);
    }

    WriteLine(StdStream::Out, text.View(), g_console.groupIndent);
    return true;
}

bool ConsoleTimeLog(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    args.rval().setUndefined();
    return ReportTimer(cx, args, false);
}

bool ConsoleTimeEnd(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    args.rval().setUndefined();
    return ReportTimer(cx, args, true);
}

bool ConsoleGroup(JSContext* cx, unsigned argc, JS::Value* vp)
{
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    args.rval().setUndefined();

    if (args.length() > 0 && !Log(cx, args, 0, StdStream::Out))
    {
        return false;
    }

    if (g_console.groupIndent < 64)
    {
        g_console.groupIndent += 2;
    }

    return true;
}

bool ConsoleGroupEnd(JSContext* cx, unsigned argc, JS::Value* vp)
{
    (void)cx;
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    args.rval().setUndefined();
    g_console.groupIndent = g_console.groupIndent >= 2 ? g_console.groupIndent - 2 : 0;
    return true;
}

const JSFunctionSpec kConsoleMethods[] = {
    JS_FN("log", ConsoleLog, 0, JSPROP_ENUMERATE),
    JS_FN("info", ConsoleLog, 0, JSPROP_ENUMERATE),
    JS_FN("debug", ConsoleLog, 0, JSPROP_ENUMERATE),
    JS_FN("dirxml", ConsoleLog, 0, JSPROP_ENUMERATE),
    JS_FN("table", ConsoleLog, 0, JSPROP_ENUMERATE),
    JS_FN("warn", ConsoleError, 0, JSPROP_ENUMERATE),
    JS_FN("error", ConsoleError, 0, JSPROP_ENUMERATE),
    JS_FN("dir", ConsoleDir, 0, JSPROP_ENUMERATE),
    JS_FN("assert", ConsoleAssert, 0, JSPROP_ENUMERATE),
    JS_FN("trace", ConsoleTrace, 0, JSPROP_ENUMERATE),
    JS_FN("count", ConsoleCount, 0, JSPROP_ENUMERATE),
    JS_FN("countReset", ConsoleCountReset, 0, JSPROP_ENUMERATE),
    JS_FN("time", ConsoleTime, 0, JSPROP_ENUMERATE),
    JS_FN("timeLog", ConsoleTimeLog, 0, JSPROP_ENUMERATE),
    JS_FN("timeEnd", ConsoleTimeEnd, 0, JSPROP_ENUMERATE),
    JS_FN("group", ConsoleGroup, 0, JSPROP_ENUMERATE),
    JS_FN("groupCollapsed", ConsoleGroup, 0, JSPROP_ENUMERATE),
    JS_FN("groupEnd", ConsoleGroupEnd, 0, JSPROP_ENUMERATE),
    JS_FS_END,
};

} // namespace

bool DefineConsole(
    JSContext* cx,
    JS::HandleObject global)
{
    JS::RootedObject console(cx, JS_NewPlainObject(cx));

    if (!console || !JS_DefineFunctions(cx, console, kConsoleMethods))
    {
        return false;
    }

    // Like every WHATWG namespace on the global: writable, configurable,
    // not enumerable.
    return JS_DefineProperty(cx, global, "console", console, 0);
}

void WriteErrorLine(
    forge::core::StringView text)
{
    WriteLine(StdStream::Err, text, 0);
}

} // namespace forge::runtime
