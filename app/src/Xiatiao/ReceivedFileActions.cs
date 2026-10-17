// SPDX-License-Identifier: MIT
using System;
using System.IO;
using System.Runtime.InteropServices;

namespace Winawdl.Xiatiao;

internal static class ReceivedFileActions
{
    internal static string RegularFile(string path)
    {
        var full = Path.GetFullPath(path);
        var attributes = File.GetAttributes(full);
        if ((attributes & (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
            throw new IOException("请选择普通文件；文件夹与链接不会被操作。");
        return full;
    }

    // Complete the copy before exposing its final name, and never overwrite an existing file.
    internal static string CopyTo(string source, string destinationFolder)
    {
        source = RegularFile(source);
        var folder = Path.GetFullPath(destinationFolder);
        if (!Directory.Exists(folder)) throw new DirectoryNotFoundException("目标文件夹不存在。");
        var temporary = Path.Combine(folder, ".airdrop-copy-" + Guid.NewGuid().ToString("N") + ".tmp");
        try
        {
            using (var input = new FileStream(source, FileMode.Open, FileAccess.Read, FileShare.Read))
            using (var output = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                input.CopyTo(output);
            var stem = Path.GetFileNameWithoutExtension(source);
            var extension = Path.GetExtension(source);
            for (var number = 0; number < 10000; number++)
            {
                var name = number == 0 ? Path.GetFileName(source) : $"{stem} ({number}){extension}";
                var destination = Path.Combine(folder, name);
                try { File.Move(temporary, destination, false); return destination; }
                catch (IOException) when (File.Exists(destination) || Directory.Exists(destination)) { }
            }
            throw new IOException("目标文件夹有过多同名文件，请选择其他文件夹。");
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
}

/// <summary>Uses Shell recycling and vetoes the permanent-delete fallback.</summary>
public static class RecycleFiles
{
    private const uint RecycleOnlyFlags = 0x00080000 | 0x00100000 | 0x00000400 | 0x00000004 | 0x00000010;

    internal static void Move(string path, IntPtr owner)
    {
        path = ReceivedFileActions.RegularFile(path);
        var root = Path.GetPathRoot(path);
        if (string.IsNullOrEmpty(root) || path.StartsWith(@"\\", StringComparison.Ordinal) ||
            new DriveInfo(root).DriveType != DriveType.Fixed)
            throw new IOException("此位置不支持安全移到回收站。文件已保留，可以只从记录中移除。");

        var operationType = Type.GetTypeFromCLSID(new Guid("3AD05575-8857-4850-9277-11B85BDB8E09"), true)!;
        var operation = (IFileOperation)Activator.CreateInstance(operationType)!;
        IntPtr item = IntPtr.Zero;
        var sink = new RecycleSink();
        try
        {
            var itemId = new Guid("43826D1E-E718-42EE-BC55-A1E261C37BFE");
            Marshal.ThrowExceptionForHR(SHCreateItemFromParsingName(path, IntPtr.Zero, ref itemId, out item));
            Marshal.ThrowExceptionForHR(operation.SetOperationFlags(RecycleOnlyFlags));
            Marshal.ThrowExceptionForHR(operation.SetOwnerWindow(owner));
            Marshal.ThrowExceptionForHR(operation.DeleteItem(item, sink));
            var result = operation.PerformOperations();
            if (sink.PermanentDeleteBlocked)
                throw new IOException("Windows 无法将这个文件放入回收站，已取消删除并保留文件。");
            Marshal.ThrowExceptionForHR(result);
            Marshal.ThrowExceptionForHR(operation.GetAnyOperationsAborted(out var aborted));
            if (aborted || !sink.Recycled)
                throw new IOException("文件没有移入回收站，已保留记录。");
        }
        finally
        {
            GC.KeepAlive(sink);
            if (item != IntPtr.Zero) Marshal.Release(item);
            Marshal.FinalReleaseComObject(operation);
        }
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode, PreserveSig = true)]
    private static extern int SHCreateItemFromParsingName(string path, IntPtr bindContext, ref Guid interfaceId, out IntPtr item);

    [ComImport, Guid("947AAB5F-0A5C-4C13-B4D6-4BF7836FC9F8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IFileOperation
    {
        [PreserveSig] int Advise(IntPtr sink, out uint cookie);
        [PreserveSig] int Unadvise(uint cookie);
        [PreserveSig] int SetOperationFlags(uint flags);
        [PreserveSig] int SetProgressMessage([MarshalAs(UnmanagedType.LPWStr)] string message);
        [PreserveSig] int SetProgressDialog(IntPtr dialog);
        [PreserveSig] int SetProperties(IntPtr properties);
        [PreserveSig] int SetOwnerWindow(IntPtr owner);
        [PreserveSig] int ApplyPropertiesToItem(IntPtr item);
        [PreserveSig] int ApplyPropertiesToItems(IntPtr items);
        [PreserveSig] int RenameItem(IntPtr item, IntPtr name, IntPtr sink);
        [PreserveSig] int RenameItems(IntPtr items, IntPtr name);
        [PreserveSig] int MoveItem(IntPtr item, IntPtr folder, IntPtr name, IntPtr sink);
        [PreserveSig] int MoveItems(IntPtr items, IntPtr folder);
        [PreserveSig] int CopyItem(IntPtr item, IntPtr folder, IntPtr name, IntPtr sink);
        [PreserveSig] int CopyItems(IntPtr items, IntPtr folder);
        [PreserveSig] int DeleteItem(IntPtr item, [MarshalAs(UnmanagedType.Interface)] IRecycleSink sink);
        [PreserveSig] int DeleteItems(IntPtr items);
        [PreserveSig] int NewItem(IntPtr folder, uint attributes, IntPtr name, IntPtr template, IntPtr sink);
        [PreserveSig] int PerformOperations();
        [PreserveSig] int GetAnyOperationsAborted([MarshalAs(UnmanagedType.Bool)] out bool aborted);
    }

    [ComVisible(true), Guid("04B0F1A7-9490-44BC-96E1-4296A31252E2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IRecycleSink
    {
        [PreserveSig] int StartOperations();
        [PreserveSig] int FinishOperations(int result);
        [PreserveSig] int PreRenameItem(uint flags, IntPtr item, IntPtr name);
        [PreserveSig] int PostRenameItem(uint flags, IntPtr item, IntPtr name, int result, IntPtr created);
        [PreserveSig] int PreMoveItem(uint flags, IntPtr item, IntPtr folder, IntPtr name);
        [PreserveSig] int PostMoveItem(uint flags, IntPtr item, IntPtr folder, IntPtr name, int result, IntPtr created);
        [PreserveSig] int PreCopyItem(uint flags, IntPtr item, IntPtr folder, IntPtr name);
        [PreserveSig] int PostCopyItem(uint flags, IntPtr item, IntPtr folder, IntPtr name, int result, IntPtr created);
        [PreserveSig] int PreDeleteItem(uint flags, IntPtr item);
        [PreserveSig] int PostDeleteItem(uint flags, IntPtr item, int result, IntPtr created);
        [PreserveSig] int PreNewItem(uint flags, IntPtr folder, IntPtr name);
        [PreserveSig] int PostNewItem(uint flags, IntPtr folder, IntPtr name, IntPtr template, uint attributes, int result, IntPtr created);
        [PreserveSig] int UpdateProgress(uint total, uint done);
        [PreserveSig] int ResetTimer();
        [PreserveSig] int PauseTimer();
        [PreserveSig] int ResumeTimer();
    }

    [ComVisible(true), ClassInterface(ClassInterfaceType.None)]
    public sealed class RecycleSink : IRecycleSink
    {
        public bool PermanentDeleteBlocked { get; private set; }
        public bool Recycled { get; private set; }
        public int PreDeleteItem(uint flags, IntPtr item)
        {
            if ((flags & 0x80) != 0) return 0; // TSF_DELETE_RECYCLE_IF_POSSIBLE
            PermanentDeleteBlocked = true;
            return unchecked((int)0x80004004); // E_ABORT, before a non-recycling delete.
        }
        public int PostDeleteItem(uint flags, IntPtr item, int result, IntPtr created)
        { Recycled = result >= 0 && created != IntPtr.Zero; return 0; }
        public int StartOperations() => 0;
        public int FinishOperations(int result) => 0;
        public int PreRenameItem(uint flags, IntPtr item, IntPtr name) => 0;
        public int PostRenameItem(uint flags, IntPtr item, IntPtr name, int result, IntPtr created) => 0;
        public int PreMoveItem(uint flags, IntPtr item, IntPtr folder, IntPtr name) => 0;
        public int PostMoveItem(uint flags, IntPtr item, IntPtr folder, IntPtr name, int result, IntPtr created) => 0;
        public int PreCopyItem(uint flags, IntPtr item, IntPtr folder, IntPtr name) => 0;
        public int PostCopyItem(uint flags, IntPtr item, IntPtr folder, IntPtr name, int result, IntPtr created) => 0;
        public int PreNewItem(uint flags, IntPtr folder, IntPtr name) => 0;
        public int PostNewItem(uint flags, IntPtr folder, IntPtr name, IntPtr template, uint attributes, int result, IntPtr created) => 0;
        public int UpdateProgress(uint total, uint done) => 0;
        public int ResetTimer() => 0;
        public int PauseTimer() => 0;
        public int ResumeTimer() => 0;
    }
}
