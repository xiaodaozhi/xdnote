{ Only manifest-listed, unchanged files below the chosen app root are retired.
  Payload deletion is controlled by this manifest, never a recursive wildcard.
  This per-user installer assumes no concurrent external directory mutation;
  path/hash rechecks are not a handle-based adversarial race boundary. }
var
  CurrentFiles, RetiredFiles: TArrayOfString;
  CleanupFailed, CleanupProcessed: Boolean;

function SafeRelativePath(const Value: String): Boolean;
var
  Rest, Part, Device: String;
  Separator, I: Integer;
begin
  Result := False;
  if (Value = '') or (Length(Value) > 1024) then exit;
  for I := 1 to Length(Value) do
    if (Ord(Value[I]) < 32) or (Pos(Value[I], '/:*?"<>|') > 0) then exit;
  if (CompareText(Value, 'install-manifest.txt') = 0) or
     (CompareText(Copy(Value, 1, 5), 'unins') = 0) or
     (CompareText(Copy(Value, 1, 9), 'versions\') = 0) then exit;
  Rest := Value;
  repeat
    Separator := Pos('\', Rest);
    if Separator = 0 then Part := Rest else Part := Copy(Rest, 1, Separator - 1);
    if (Part = '') or (Part = '.') or (Part = '..') or
       (Part[Length(Part)] = '.') or (Part[Length(Part)] = ' ') then exit;
    Device := Lowercase(Part);
    I := Pos('.', Device);
    if I > 0 then Device := Copy(Device, 1, I - 1);
    if (Device = 'con') or (Device = 'prn') or (Device = 'aux') or
       (Device = 'nul') or (Device = 'clock$') then exit;
    if (Length(Device) = 4) and
       ((Copy(Device, 1, 3) = 'com') or (Copy(Device, 1, 3) = 'lpt')) and
       (Pos(Device[4], '123456789') > 0) then exit;
    if Separator = 0 then break;
    Delete(Rest, 1, Separator);
  until False;
  Result := True;
end;

function SafeFileChain(const Name: String): Boolean;
var
  Root, Cursor: String;
  Info: TFindRec;
begin
  Result := False;
  Root := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));
  Cursor := Name;
  while True do begin
    if FindFirst(Cursor, Info) then begin
      FindClose(Info);
      if (Info.Attributes and FILE_ATTRIBUTE_REPARSE_POINT) <> 0 then exit;
    end;
    if CompareText(Cursor, Root) = 0 then break;
    Cursor := ExtractFileDir(Cursor);
    if Length(Cursor) < Length(Root) then exit;
  end;
  Result := True;
end;

function EntryPath(const Entry: String): String;
begin
  Result := Copy(Entry, 66, MaxInt);
end;

function ValidEntry(const Entry: String): Boolean;
var I: Integer;
begin
  Result := False;
  if Pos(#9, Entry) <> 65 then exit;
  for I := 1 to 64 do
    if Pos(Lowercase(Entry[I]), '0123456789abcdef') = 0 then exit;
  Result := SafeRelativePath(EntryPath(Entry));
end;

function ReadManifest(const Name: String; var Entries: TArrayOfString): Boolean;
var I, J, Size: Integer;
begin
  Result := False;
  if not SafeFileChain(Name) then exit;
  if not FileSize(Name, Size) then exit;
  if (Size < 1) or (Size > 2097152) then exit;
  if not LoadStringsFromFile(Name, Entries) then exit;
  if (GetArrayLength(Entries) < 1) or (GetArrayLength(Entries) > 16385) then exit;
  if Entries[0] <> 'xdnote-install-files-v1' then exit;
  for I := 1 to GetArrayLength(Entries) - 1 do begin
    if not ValidEntry(Entries[I]) then exit;
    for J := 1 to I - 1 do
      if CompareText(EntryPath(Entries[I]), EntryPath(Entries[J])) = 0 then exit;
  end;
  Result := True;
end;

function IsCurrentFile(const RelativePath: String): Boolean;
var I: Integer;
begin
  Result := False;
  for I := 1 to GetArrayLength(CurrentFiles) - 1 do
    if CompareText(RelativePath, EntryPath(CurrentFiles[I])) = 0 then begin
      Result := True;
      exit;
    end;
end;

function UnchangedFile(const Name, Entry: String): Boolean;
begin
  Result := False;
  if not SafeFileChain(Name) then exit;
  try
    Result := CompareText(GetSHA256OfFile(Name), Copy(Entry, 1, 64)) = 0;
  except
    Result := False;
  end;
end;

function DirectoryEmpty(const Name: String): Boolean;
var Info: TFindRec;
begin
  Result := False;
  if FindFirst(AddBackslash(Name) + '*', Info) then begin
    Result := True;
    try
      repeat
        if (Info.Name <> '.') and (Info.Name <> '..') then begin Result := False; break; end;
      until not FindNext(Info);
    finally
      FindClose(Info);
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Previous: TArrayOfString;
  I, Count: Integer;
  Name, Manifest: String;
begin
  Result := '';
  SetArrayLength(RetiredFiles, 0);
  ExtractTemporaryFile('xdnote-package-files.txt');
  { This immutable copy is inside the installer, not read from the destination. }
  if not LoadStringsFromFile(ExpandConstant('{tmp}\xdnote-package-files.txt'), CurrentFiles) then begin
    Result := CustomMessage('InvalidInstalledFiles');
    exit;
  end;
  Manifest := ExpandConstant('{app}\install-manifest.txt');
  if not SafeFileChain(ExpandConstant('{app}')) then begin
    Result := CustomMessage('InvalidInstalledFiles'); exit;
  end;
  if not FileOrDirExists(Manifest) then begin
    if DirExists(ExpandConstant('{app}')) and not DirectoryEmpty(ExpandConstant('{app}')) then
      Result := CustomMessage('InvalidInstalledFiles');
    exit;
  end;
  if not ReadManifest(Manifest, Previous) then begin
    Result := CustomMessage('InvalidInstalledFiles');
    exit;
  end;
  { Verify every owned file, including files which the upgrade will replace. }
  for I := 1 to GetArrayLength(Previous) - 1 do begin
    Name := ExpandConstant('{app}\') + EntryPath(Previous[I]);
    if FileOrDirExists(Name) and not UnchangedFile(Name, Previous[I]) then begin
      Result := CustomMessage('ModifiedInstalledFiles'); exit;
    end;
    if not IsCurrentFile(EntryPath(Previous[I])) then begin
      Name := ExpandConstant('{app}\') + EntryPath(Previous[I]);
      if FileOrDirExists(Name) then begin
        if not UnchangedFile(Name, Previous[I]) then begin
          Result := CustomMessage('ModifiedInstalledFiles');
          exit;
        end;
        Count := GetArrayLength(RetiredFiles);
        SetArrayLength(RetiredFiles, Count + 1);
        RetiredFiles[Count] := Previous[I];
      end;
    end;
  end;
  for I := 1 to GetArrayLength(CurrentFiles) - 1 do begin
    Name := ExpandConstant('{app}\') + EntryPath(CurrentFiles[I]);
    if not SafeFileChain(Name) then begin
      Result := CustomMessage('InvalidInstalledFiles'); exit;
    end;
    if FileOrDirExists(Name) and not UnchangedFile(Name, CurrentFiles[I]) then begin
      Count := 1;
      while (Count < GetArrayLength(Previous)) and
        (CompareText(EntryPath(CurrentFiles[I]), EntryPath(Previous[Count])) <> 0) do Count := Count + 1;
      if Count = GetArrayLength(Previous) then begin
        Result := CustomMessage('ModifiedInstalledFiles'); exit;
      end;
    end;
  end;
end;

procedure RemoveEmptyParents(Name: String);
var Root: String;
begin
  Root := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));
  Name := ExtractFileDir(Name);
  while (Length(Name) > Length(Root)) and SafeFileChain(Name) do begin
    if not RemoveDir(Name) then break;
    Name := ExtractFileDir(Name);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var I, Count: Integer; Name: String; Removed: Boolean;
begin
  if CurStep <> ssDone then exit;
  { Inno can call event functions more than once during one Setup run. }
  if CleanupProcessed then exit;
  CleanupProcessed := True;
  for I := 0 to GetArrayLength(RetiredFiles) - 1 do begin
    Name := ExpandConstant('{app}\') + EntryPath(RetiredFiles[I]);
    Removed := False;
    if UnchangedFile(Name, RetiredFiles[I]) then begin
      Removed := DeleteFile(Name);
      if not Removed then Log('Could not delete an unchanged obsolete manifest-owned file.');
    end else
      Log('An obsolete manifest-owned file could not be reverified at cleanup.');
    if Removed then
      RemoveEmptyParents(Name)
    else begin
      { Keep a retry record if the file changed or became locked after preflight. }
      CleanupFailed := True;
      Count := GetArrayLength(CurrentFiles);
      SetArrayLength(CurrentFiles, Count + 1);
      CurrentFiles[Count] := RetiredFiles[I];
      Log('An obsolete manifest-owned file was preserved; cleanup must be retried.');
    end;
  end;
  if CleanupFailed then begin
    if not SaveStringsToUTF8File(ExpandConstant('{app}\install-manifest.txt'), CurrentFiles, False) then
      Log('Could not preserve the cleanup retry record; manual repair is required.');
    SuppressibleMsgBox(CustomMessage('InstalledFilesCleanupFailed'), mbError, MB_OK, IDOK);
  end;
end;

function GetCustomSetupExitCode(): Integer;
begin
  Result := 0;
  if CleanupFailed then Result := 20;
end;
