{
  A3kPack.pas

  Reference Delphi unit that reproduces the Yamaha A3000 ".a3k" archive
  container ("A3kDiskyPC") produced by the lost A3kDisky program.

  The .a3k file is a hand-rolled, record-based binary format written through
  a TFileStream (the standard Delphi 'Classes' unit container).  It is NOT a
  zip/lzh/arc archive; it stores raw SFS file images.

  On-disk layout
  --------------
    [0:1110]   TA3kHeader   (version, dataSize, fileCount, magic, sentinel)
    [1110:..]  banner text  (the "/A3kFileInfo.txt" entry content)
    [..:dsz]   SFS file images (each begins with "FSFSDEV3SPLX" + type)
    [dsz:EOF]  array of TA3kFileInfo (file_count * 271 bytes)

  This unit is provided for reference only; it is not part of the nDISKY build.
}
unit A3kPack;

interface

uses
  System.SysUtils, System.Classes;

const
  A3K_HEADER_SIZE = 1110;                       // bytes in TA3kHeader
  A3K_ENTRY_SIZE  = 271;                        // bytes in TA3kFileInfo
  A3K_MAGIC       = 'A3kDiskyPC';               // 10 chars
  A3K_SENTINEL    = 'XXXXXXXXXXXXXXXX';          // 16 chars
  A3K_FILEINFO    = '/A3kFileInfo.txt';         // banner entry path

  SFS_MAGIC       = 'FSFSDEV3SPLX';             // 12 chars, SFS file image

type
  // ---------------------------------------------------------------------
  // 1110-byte file header
  // ---------------------------------------------------------------------
  TA3kHeader = packed record
    Version:   LongInt;                     // $00000001
    DataSize:  LongInt;                     // start offset of file-info section
    FileCount: LongInt;                     // number of file-info entries
    Magic:     array[0..9] of AnsiChar;     // 'A3kDiskyPC'
    Reserved:  array[0..47] of Byte;        // padding
    Sentinel:  array[0..15] of AnsiChar;    // 'XXXXXXXXXXXXXXXX'
    Reserved2: array[0..1023] of Byte;       // padding
  end;

  // ---------------------------------------------------------------------
  // 271-byte file-info entry (one per item in the archive)
  // ---------------------------------------------------------------------
  TA3kFileInfo = packed record
    Path:   array[0..255] of AnsiChar;      // "VolumeName \TYPE\FileName"
    Marker: Word;                           // $0001 file / $0000 banner
    Offset: LongInt;                        // absolute offset in the file
    Size:   LongInt;                        // size of the entry
    Flags:  LongInt;                        // $00000001
    Pad:    Byte;                           // $00
  end;

  // A single archived item: its path plus the full SFS file-image bytes.
  TA3kEntry = record
    Path: string;                           // "VolumeName \TYPE\FileName"
    Data: TBytes;                           // full SFS file-image bytes
  end;

  // SFS file-image types
  TA3kSfsType = (sfsProg, sfsSbac, sfsSbnk, sfsSmpl, sfsSequ, sfsUnknown);

  // ---------------------------------------------------------------------
  // High-level archive container
  // ---------------------------------------------------------------------
  TA3kArchive = class
  private
    FBanner:  AnsiString;                    // description text (entry 0)
    FEntries: array of TA3kEntry;            // archived files
    function GetEntryCount: Integer;
    function GetEntry(Index: Integer): TA3kEntry;
  public
    constructor Create;

    // Build a path string "VolumeName \TYPE\FileName"
    class function MakePath(const VolumeName, SfsType, FileName: string): string;

    // Add a file entry from raw SFS file-image bytes.
    procedure AddFile(const Path: string; const Data: TBytes);

    // Read / write the whole archive from / to a stream.
    procedure LoadFromStream(Stream: TStream);
    procedure SaveToStream(Stream: TStream);
    procedure LoadFromFile(const FileName: string);
    procedure SaveToFile(const FileName: string);

    property Banner: AnsiString read FBanner write FBanner;
    property EntryCount: Integer read GetEntryCount;
    property Entries[Index: Integer]: TA3kEntry read GetEntry;
  end;

  // ---------------------------------------------------------------------
  // Big-endian helpers for parsing the SFS file-image headers
  // ---------------------------------------------------------------------
function BE32(const Buf: PByte): LongInt;
function BE16(const Buf: PByte): Word;
function SfsTypeFromMagic(const Buf: PByte): TA3kSfsType;
function SfsTypeName(AType: TA3kSfsType): string;

implementation

// ---------------------------------------------------------------------------
// Big-endian readers (the A3000 / SFS on-disk headers are big-endian)
// ---------------------------------------------------------------------------
function BE32(const Buf: PByte): LongInt;
begin
  Result := (LongInt(Buf[0]) shl 24) or (LongInt(Buf[1]) shl 16)
          or (LongInt(Buf[2]) shl 8) or LongInt(Buf[3]);
end;

function BE16(const Buf: PByte): Word;
begin
  Result := (Word(Buf[0]) shl 8) or Word(Buf[1]);
end;

function SfsTypeFromMagic(const Buf: PByte): TA3kSfsType;
var
  T: array[0..3] of AnsiChar;
begin
  // Buf points at the 4-byte type field that follows "FSFSDEV3SPLX"
  T[0] := AnsiChar(Buf[0]); T[1] := AnsiChar(Buf[1]);
  T[2] := AnsiChar(Buf[2]); T[3] := AnsiChar(Buf[3]);
  if      (T = 'PROG') then Result := sfsProg
  else if (T = 'SBAC') then Result := sfsSbac
  else if (T = 'SBNK') then Result := sfsSbnk
  else if (T = 'SMPL') then Result := sfsSmpl
  else if (T = 'SEQU') then Result := sfsSequ
  else Result := sfsUnknown;
end;

function SfsTypeName(AType: TA3kSfsType): string;
begin
  case AType of
    sfsProg: Result := 'PROG';
    sfsSbac: Result := 'SBAC';
    sfsSbnk: Result := 'SBNK';
    sfsSmpl: Result := 'SMPL';
    sfsSequ: Result := 'SEQU';
  else     Result := '????';
  end;
end;

// ---------------------------------------------------------------------------
// TA3kArchive
// ---------------------------------------------------------------------------
constructor TA3kArchive.Create;
begin
  inherited Create;
  FBanner := '';
  SetLength(FEntries, 0);
end;

function TA3kArchive.GetEntryCount: Integer;
begin
  Result := Length(FEntries);
end;

function TA3kArchive.GetEntry(Index: Integer): TA3kEntry;
begin
  Result := FEntries[Index];
end;

class function TA3kArchive.MakePath(const VolumeName, SfsType, FileName: string): string;
var
  Vol, Fn: string;
begin
  // Volume name and file name are space-padded to 16 chars in the path field.
  Vol := Copy(VolumeName + '                ', 1, 16);
  Fn  := Copy(FileName   + '                ', 1, 16);
  Result := Vol + '\' + SfsType + '\' + Fn;
end;

procedure TA3kArchive.AddFile(const Path: string; const Data: TBytes);
var
  N: Integer;
begin
  N := Length(FEntries);
  SetLength(FEntries, N + 1);
  FEntries[N].Path := Path;
  FEntries[N].Data := Data;
end;

procedure TA3kArchive.SaveToStream(Stream: TStream);
var
  Header: TA3kHeader;
  E: TA3kFileInfo;
  I: Integer;
  Off: LongInt;
begin
  // Compute dsz = start of the file-info section.
  Off := A3K_HEADER_SIZE + Length(FBanner);
  for I := 0 to Length(FEntries) - 1 do
    Inc(Off, Length(FEntries[I].Data));

  // Build and write the header.
  FillChar(Header, SizeOf(Header), 0);
  Header.Version   := 1;
  Header.DataSize  := Off;
  Header.FileCount := Length(FEntries) + 1;      // +1 for the banner entry
  StrPCopy(@Header.Magic[0], A3K_MAGIC);
  StrPCopy(@Header.Sentinel[0], A3K_SENTINEL);
  Stream.WriteBuffer(Header, SizeOf(Header));

  // Write the banner text.
  if Length(FBanner) > 0 then
    Stream.WriteBuffer(PAnsiChar(FBanner)^, Length(FBanner));

  // Write each SFS file image (raw bytes).
  for I := 0 to Length(FEntries) - 1 do
    if Length(FEntries[I].Data) > 0 then
      Stream.WriteBuffer(FEntries[I].Data[0], Length(FEntries[I].Data));

  // Write the file-info section: banner entry first, then each file entry.
  FillChar(E, SizeOf(E), 0);
  StrPCopy(@E.Path[0], A3K_FILEINFO);
  E.Marker := $0000;                        // banner entry
  E.Offset := A3K_HEADER_SIZE;
  E.Size   := Length(FBanner);
  E.Flags  := $00000001;
  Stream.WriteBuffer(E, SizeOf(E));

  Off := A3K_HEADER_SIZE + Length(FBanner);
  for I := 0 to Length(FEntries) - 1 do
  begin
    FillChar(E, SizeOf(E), 0);
    StrPLCopy(@E.Path[0], FEntries[I].Path, 255);
    E.Marker := $0001;                      // file entry
    E.Offset := Off;
    E.Size   := Length(FEntries[I].Data);
    E.Flags  := $00000001;
    Stream.WriteBuffer(E, SizeOf(E));
    Inc(Off, Length(FEntries[I].Data));
  end;
end;

procedure TA3kArchive.LoadFromStream(Stream: TStream);
var
  Header: TA3kHeader;
  Info:   array of TA3kFileInfo;
  I: Integer;
  BannerLen: LongInt;
begin
  Stream.ReadBuffer(Header, SizeOf(Header));
  SetLength(Info, Header.FileCount);

  // Read the whole file-info section.
  Stream.Position := Header.DataSize;
  for I := 0 to Header.FileCount - 1 do
    Stream.ReadBuffer(Info[I], SizeOf(TA3kFileInfo));

  // Banner (entry 0).
  BannerLen := Info[0].Size;
  SetLength(FBanner, BannerLen);
  Stream.Position := Info[0].Offset;
  if BannerLen > 0 then
    Stream.ReadBuffer(PAnsiChar(FBanner)^, BannerLen);

  // Each file entry's raw bytes.
  SetLength(FEntries, Header.FileCount - 1);
  for I := 1 to Header.FileCount - 1 do
  begin
    FEntries[I - 1].Path := StrPas(@Info[I].Path[0]);
    SetLength(FEntries[I - 1].Data, Info[I].Size);
    Stream.Position := Info[I].Offset;
    if Info[I].Size > 0 then
      Stream.ReadBuffer(FEntries[I - 1].Data[0], Info[I].Size);
  end;
end;

procedure TA3kArchive.LoadFromFile(const FileName: string);
var
  FS: TFileStream;
begin
  FS := TFileStream.Create(FileName, fmOpenRead or fmShareDenyWrite);
  try
    LoadFromStream(FS);
  finally
    FS.Free;
  end;
end;

procedure TA3kArchive.SaveToFile(const FileName: string);
var
  FS: TFileStream;
begin
  FS := TFileStream.Create(FileName, fmCreate);
  try
    SaveToStream(FS);
  finally
    FS.Free;
  end;
end;

end.
