// BunkerVR patch format, version 1.
//
// A patch turns one of the game's own files into the mod's version of it without carrying any of the game's
// text. It lists how many of the original's lines to keep, how many to drop, and the mod's lines to insert.
// The original and the result are both identified by SHA-256 and size, so a patch applies only to the exact
// file it was made from, and what it produces is checked before it is used.
//
//   BunkerVR patch 1\n
//   file <path inside the game folder, '/' between folders>\n
//   from <sha256 hex> <size>\n       the game's file
//   to <sha256 hex> <size>\n         the mod's file
//   =<n>\n                           keep the next n lines of the original
//   -<n>\n                           drop the next n lines of the original
//   +<line>                          insert one line exactly as it is in the result, with its own line ending
//   \\n                              (right after a + line) that line has no line ending: the result ends there
//   end\n                            whatever is left of the original is kept
//
// Lines are counted by '\n'. A '\r' before it is part of the line, so CRLF and LF files both come out exact.

using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text;

public static class BunkerVRPatch
{
    public const string Magic = "BunkerVR patch 1";

    public sealed class Info
    {
        public string File;
        public string FromSha256;
        public long FromSize;
        public string ToSha256;
        public long ToSize;
    }

    public static string Sha256Hex(byte[] data)
    {
        using (var sha = SHA256.Create())
        {
            var hash = sha.ComputeHash(data);
            var sb = new StringBuilder(64);
            foreach (byte b in hash) sb.Append(b.ToString("x2"));
            return sb.ToString();
        }
    }

    // Splits into lines, each keeping its '\n' (the last one may have none).
    public static List<ArraySegment<byte>> Lines(byte[] data)
    {
        var lines = new List<ArraySegment<byte>>();
        int start = 0;
        for (int i = 0; i < data.Length; i++)
        {
            if (data[i] == (byte)'\n') { lines.Add(new ArraySegment<byte>(data, start, i + 1 - start)); start = i + 1; }
        }
        if (start < data.Length) lines.Add(new ArraySegment<byte>(data, start, data.Length - start));
        return lines;
    }

    // One change: keep `Keep` original lines, drop `Drop`, insert `Insert` (lines of the result, endings included).
    public sealed class Hunk
    {
        public int Keep;
        public int Drop;
        public List<ArraySegment<byte>> Insert = new List<ArraySegment<byte>>();
    }

    public static byte[] Write(string path, byte[] original, byte[] result, List<Hunk> hunks)
    {
        var o = new MemoryStream();
        Action<string> text = s => { var b = Encoding.UTF8.GetBytes(s); o.Write(b, 0, b.Length); };
        text(Magic + "\n");
        text("file " + path + "\n");
        text("from " + Sha256Hex(original) + " " + original.LongLength + "\n");
        text("to " + Sha256Hex(result) + " " + result.LongLength + "\n");
        foreach (var h in hunks)
        {
            if (h.Keep > 0) text("=" + h.Keep + "\n");
            if (h.Drop > 0) text("-" + h.Drop + "\n");
            foreach (var line in h.Insert)
            {
                o.WriteByte((byte)'+');
                o.Write(line.Array, line.Offset, line.Count);
                if (line.Count == 0 || line.Array[line.Offset + line.Count - 1] != (byte)'\n') text("\n\\\n");
            }
        }
        text("end\n");
        return o.ToArray();
    }

    // Reads the header only.
    public static Info ReadInfo(byte[] patch)
    {
        int at = 0;
        return ReadHeader(patch, ref at);
    }

    static string NextTextLine(byte[] data, ref int at)
    {
        int end = Array.IndexOf(data, (byte)'\n', at);
        if (end < 0) throw new InvalidDataException("the patch ends in the middle of a line");
        string s = Encoding.UTF8.GetString(data, at, end - at);
        at = end + 1;
        return s;
    }

    static Info ReadHeader(byte[] patch, ref int at)
    {
        if (NextTextLine(patch, ref at) != Magic) throw new InvalidDataException("not a BunkerVR patch (version 1)");
        var info = new Info();
        string file = NextTextLine(patch, ref at);
        string from = NextTextLine(patch, ref at);
        string to = NextTextLine(patch, ref at);
        if (!file.StartsWith("file ") || !from.StartsWith("from ") || !to.StartsWith("to ")) throw new InvalidDataException("the patch's header is damaged");
        info.File = file.Substring(5);
        var f = from.Substring(5).Split(' ');
        var t = to.Substring(3).Split(' ');
        if (f.Length != 2 || t.Length != 2) throw new InvalidDataException("the patch's header is damaged");
        info.FromSha256 = f[0]; info.FromSize = long.Parse(f[1]);
        info.ToSha256 = t[0]; info.ToSize = long.Parse(t[1]);
        if (info.File.Length == 0 || info.File.Contains("..") || info.File.Contains(":") || info.File.StartsWith("/"))
            throw new InvalidDataException("the patch names an unsafe path");
        return info;
    }

    // Applies a patch to the game's file. Throws if the file is not the one the patch was made from, if the
    // patch is damaged, or if the result is not exactly the mod's file.
    public static byte[] Apply(byte[] original, byte[] patch)
    {
        int at = 0;
        Info info = ReadHeader(patch, ref at);
        if (original.LongLength != info.FromSize || Sha256Hex(original) != info.FromSha256)
            throw new InvalidDataException(info.File + " is not the version of the game's file this patch was made for");
        var lines = Lines(original);
        var o = new MemoryStream(original.Length + patch.Length);
        int cursor = 0;
        int lastInsertEnd = -1; // where the last inserted line ended in `o`, for a following "\" line
        while (true)
        {
            if (at >= patch.Length) throw new InvalidDataException("the patch ends without its \"end\" line");
            byte kind = patch[at];
            if (kind == (byte)'+')
            {
                int end = Array.IndexOf(patch, (byte)'\n', at);
                if (end < 0) throw new InvalidDataException("the patch ends in the middle of a line");
                o.Write(patch, at + 1, end - at);
                lastInsertEnd = (int)o.Length;
                at = end + 1;
                continue;
            }
            string op = NextTextLine(patch, ref at);
            if (op == "end") break;
            if (op == "\\")
            {
                if (lastInsertEnd != (int)o.Length) throw new InvalidDataException("the patch's line-ending mark is out of place");
                o.SetLength(o.Length - 1); // the '\n' that only ended the patch's own line
                continue;
            }
            int n;
            if (op.Length < 2 || !int.TryParse(op.Substring(1), out n) || n <= 0) throw new InvalidDataException("the patch has an unknown line: " + op);
            if (cursor + n > lines.Count) throw new InvalidDataException("the patch reaches past the end of " + info.File);
            if (op[0] == '=') { for (int k = 0; k < n; k++) { var l = lines[cursor + k]; o.Write(l.Array, l.Offset, l.Count); } }
            else if (op[0] != '-') throw new InvalidDataException("the patch has an unknown line: " + op);
            cursor += n;
        }
        for (; cursor < lines.Count; cursor++) { var l = lines[cursor]; o.Write(l.Array, l.Offset, l.Count); }
        byte[] result = o.ToArray();
        if (result.LongLength != info.ToSize || Sha256Hex(result) != info.ToSha256)
            throw new InvalidDataException("patching " + info.File + " did not give the expected result");
        return result;
    }
}
