// SPDX-License-Identifier: MIT
using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using Winawdl.Xiatiao;

internal static class FileInteractionChecks
{
    private static readonly List<string> Results = new();
    [STAThread]
    public static int Main(string[] args)
    {
        var root = Path.GetFullPath(args.Single());
        Directory.CreateDirectory(root);
        try
        {
            _ = new Application();
            CheckSending(root);
            CheckDrops(root);
            CheckDragOut(root);
            File.WriteAllText(Path.Combine(root, "checks.json"), JsonSerializer.Serialize(new { passed = true, checks = Results, desktopInputSent = false, radioStarted = false }, new JsonSerializerOptions { WriteIndented = true }));
            Console.WriteLine($"PASS: {Results.Count} checks"); return 0;
        }
        catch (Exception e)
        {
            File.WriteAllText(Path.Combine(root, "checks.json"), JsonSerializer.Serialize(new { passed = false, checks = Results, error = e.ToString() }));
            Console.Error.WriteLine(e); return 1;
        }
    }
    private static void Require(bool value, string message)
    {
        if (!value) throw new Exception(message);
        Results.Add(message);
    }
    private static void CheckSending(string root)
    {
        var first = Path.Combine(root, "first.txt"); var second = Path.Combine(root, "second.txt");
        File.WriteAllText(first, "first payload"); File.WriteAllText(second, "second payload");
        var commands = new List<JsonElement>(); var notifications = 0;
        var sender = new SenderController(value => commands.Add(JsonSerializer.SerializeToElement(value)), (_, _) => notifications++);
        void Event(string id, string state) => sender.HandleEvent(JsonSerializer.SerializeToElement(new { @event = "send", id, state, done = 100, total = 100 }));
        string Start()
        {
            sender.SendCommand.Execute(null);
            return commands.Last().GetProperty("id").GetString()!;
        }
        sender.SetReady(true); sender.BeginLooking();
        sender.HandleEvent(JsonSerializer.SerializeToElement(new { @event = "peer", id = "fixture-peer", name = "Fixture", model = "iPhone" }));
        sender.SelectedPeer = sender.Peers.Single();
        sender.AddFiles(new[] { first, second });
        var id = Start();
        Event("foreign-id", "sent");
        Require(sender.IsSending && sender.Files.Count == 2, "Foreign completion cannot clear the active batch");
        Event(id, "finishing");
        Require(sender.IsSending && sender.Files.Count == 2 && notifications == 0, "Upload completion retains files until receiver acknowledgement");
        Event(id, "sent");
        Require(!sender.IsSending && sender.Files.Count == 0 && notifications == 1, "Acknowledged batch automatically leaves the outgoing queue");
        Require(File.ReadAllText(first) == "first payload" && File.ReadAllText(second) == "second payload", "Successful send preserves source files");
        sender.AddFiles(new[] { second });
        Event(id, "sent");
        Require(sender.Files.Count == 1 && notifications == 1, "Late completion cannot clear newly queued files");
        foreach (var state in new[] { "cancelled", "declined", "refused", "failed" })
        {
            id = Start(); Event(id, state);
            Require(!sender.IsSending && sender.Files.Count == 1 && notifications == 1, $"{state} retains the outgoing files for retry");
        }
        id = Start();
        sender.Files.Add(new OutgoingFile { FullPath = first, Bytes = 13 });
        Event(id, "sent");
        Require(sender.Files.Count == 1 && sender.Files[0].FullPath == first, "Completion removes the submitted snapshot, not unrelated queued files");
        sender.Files.Clear(); sender.AddFiles(new[] { second }); notifications = 1;
        id = Start(); sender.SetReady(false);
        Event(id, "sent");
        Require(!sender.IsSending && sender.Files.Count == 1 && notifications == 1, "Connection loss retains the queue and ignores late success");
    }

    private static DataObject FileData(params string[] files) => new(DataFormats.FileDrop, files);
    private static void CheckDrops(string root)
    {
        var commands = new List<JsonElement>();
        var sender = new SenderController(value => commands.Add(JsonSerializer.SerializeToElement(value)), (_, _) => { });
        var first = Path.Combine(root, "first.txt"); var second = Path.Combine(root, "second.txt");
        var opened = 0;
        var dropped = FileDropInput.Receive(sender, FileData(first, second), DragDropEffects.Copy | DragDropEffects.Move, () => opened++);
        Require(dropped == DragDropEffects.Copy && opened == 1 && sender.Files.Select(f => f.FullPath).SequenceEqual(new[] { first, second }), "External multi-file drop queues real paths and opens sender");
        FileDropInput.Receive(sender, FileData(first), DragDropEffects.Copy);
        Require(sender.Files.Count == 2, "Repeated drop deduplicates file paths");
        Require(FileDropInput.Receive(sender, new DataObject(DataFormats.Text, first), DragDropEffects.Copy) == DragDropEffects.None, "Plain text is not treated as an Explorer file drop");
        Require(FileDropInput.Receive(sender, FileData(first), DragDropEffects.Move) == DragDropEffects.None, "Move-only drag is not accepted as a copy");
        var duplicateFolder = Path.Combine(root, "duplicate"); Directory.CreateDirectory(duplicateFolder);
        var duplicate = Path.Combine(duplicateFolder, "first.txt"); File.WriteAllText(duplicate, "duplicate name");
        var third = Path.Combine(root, "third.txt"); File.WriteAllText(third, "third payload");
        Require(FileDropInput.Receive(sender, FileData(third, duplicate), DragDropEffects.Copy) == DragDropEffects.None && sender.Files.Count == 2, "Invalid filename collision rejects the whole dropped batch");
        Require(FileDropInput.Receive(sender, FileData(third, root), DragDropEffects.Copy) == DragDropEffects.None && sender.Files.Count == 2, "Folder mixed with files leaves the queue unchanged");
        var own = FileData(third); own.SetData(ReceivedFileDrag.SourceFormat, true);
        Require(FileDropInput.Receive(sender, own, DragDropEffects.Copy, () => opened++) == DragDropEffects.None && opened == 1, "Dragging out over our main window does not reopen sender");
        Require(FileDropInput.Receive(sender, own, DragDropEffects.Copy) == DragDropEffects.Copy && sender.Files.Count == 3, "Received files can be dropped into an already open send window");
        sender.SetReady(true); sender.BeginLooking();
        sender.HandleEvent(JsonSerializer.SerializeToElement(new { @event = "peer", id = "peer", name = "Fixture" }));
        sender.SelectedPeer = sender.Peers.Single(); sender.SendCommand.Execute(null);
        Require(!FileDropInput.Accepts(sender, FileData(first), DragDropEffects.Copy, false), "Busy sender advertises no-drop feedback");
        Require(FileDropInput.Receive(sender, FileData(first), DragDropEffects.Copy) == DragDropEffects.None && sender.Files.Count == 3, "Drop during transmission cannot mutate the batch");
        sender.HandleEvent(JsonSerializer.SerializeToElement(new { @event = "send", id = commands.Last().GetProperty("id").GetString(), state = "sent" }));
        Require(sender.Files.Count == 0, "Dropped files use the same acknowledged-send queue cleanup");
        var window = new SendWindow(sender);
        Require(window.AllowDrop, "The actual send window enables Windows drops");
        window.Close();
    }

    private static void CheckDragOut(string root)
    {
        using var model = new AppController(true);
        foreach (var name in new[] { "first.txt", "second.txt" })
            model.Files.Add(new ReceivedItem { FullPath = Path.Combine(root, name), Bytes = 13, ReceivedAt = DateTimeOffset.Now });
        var window = new XiatiaoWindow(model);
        AppChecks.Save(window, 860, 900, Path.Combine(root, "received-drag-ready.png"));
        Require(window.AllowDrop, "The actual main window enables Windows drops");
        var list = (ListBox)window.FindName("FileList");
        var tile = (ListBoxItem)list.ItemContainerGenerator.ContainerFromIndex(0);
        var drag = new ReceivedFileDrag(list, error => throw new Exception(error));
        var origin = new Point(10, 10); var far = new Point(10 + SystemParameters.MinimumHorizontalDragDistance + 2, 10);
        list.SelectAll();
        Require(drag.Begin(tile, origin, ModifierKeys.None, 1), "Press on selected tile defers collapse of multi-selection");
        Require(drag.TakePayload(new Point(11, 10), true) is null && list.SelectedItems.Count == 2, "Small mouse movement keeps selection without starting drag");
        var data = drag.TakePayload(far, true)!;
        Require(data is not null && data.GetFileDropList().Cast<string>().SequenceEqual(new[] { Path.Combine(root, "first.txt"), Path.Combine(root, "second.txt") }), "Multi-selection creates native CF_HDROP with actual full file paths");
        Require(!drag.EndClick() && list.SelectedItems.Count == 2 && model.Files.All(f => File.Exists(f.FullPath)), "Finished drag preserves selection, history and original files");
        drag.Begin(tile, origin, ModifierKeys.None, 1); drag.EndClick();
        Require(list.SelectedItems.Count == 1 && list.SelectedItem == model.Files[0], "Click without drag still collapses selection normally");
        drag.Begin(tile, origin, ModifierKeys.None, 1);
        Require(drag.TakePayload(far, false) is null, "Released button cannot start a stale drag");
        Require(!drag.Begin(list, origin, ModifierKeys.None, 1) && drag.TakePayload(far, true) is null, "Blank-space gestures stay available for marquee selection");
        Require(!drag.Begin(tile, origin, ModifierKeys.None, 2) && drag.TakePayload(far, true) is null, "Double-click remains open-file interaction");
        Require(!drag.Begin(tile, origin, ModifierKeys.Control, 1), "Ctrl selection is left to normal list selection handling");
        drag.EndClick(); list.SelectAll();
        var missing = new ReceivedItem { FullPath = Path.Combine(root, "missing.txt") }; model.Files.Add(missing); list.SelectAll();
        drag.Begin(tile, origin, ModifierKeys.None, 1);
        try { drag.TakePayload(far, true); throw new Exception("Missing source was advertised as draggable"); }
        catch (FileNotFoundException) { Results.Add("Missing source prevents a misleading partial file drag"); }
        window.Close();
    }
}
