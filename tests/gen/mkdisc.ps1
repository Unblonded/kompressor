param([string]$Dir, [string]$Out, [int]$Fs = 4, [int]$Rev = 0)
# Builds a disc image of $Dir with Windows' IMAPI2 (Fs: 1 ISO9660, 2 Joliet, 4 UDF). Rev: UDF revision (e.g. 0x102, 0x150, 0x201, 0x250, 0x260).
if (-not ('StreamSaver' -as [type])) {
Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
public static class StreamSaver {
    public static void Save(object comStream, string path) {
        IStream s = (IStream)comStream;
        using (FileStream f = File.Create(path)) {
            byte[] buf = new byte[1 << 20];
            IntPtr read = Marshal.AllocHGlobal(4);
            try {
                while (true) {
                    s.Read(buf, buf.Length, read);
                    int n = Marshal.ReadInt32(read);
                    if (n <= 0) break;
                    f.Write(buf, 0, n);
                }
            } finally { Marshal.FreeHGlobal(read); }
        }
    }
}
"@
}
$fsi = New-Object -ComObject IMAPI2FS.MsftFileSystemImage
$fsi.FileSystemsToCreate = $Fs
if ($Rev -ne 0) { $fsi.UDFRevision = $Rev }
$fsi.FreeMediaBlocks = 0
$fsi.VolumeName = "KZTEST"
$fsi.Root.AddTree($Dir, $false)
$img = $fsi.CreateResultImage()
[StreamSaver]::Save($img.ImageStream, $Out)
