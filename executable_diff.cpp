/* This file is Copyright 2002 Level Control Systems.  See the included LICENSE.txt file for details. */  

#include "dataio/FileDataIO.h"
#include "system/SetupSystem.h"
#include "system/SystemInfo.h"
#include "util/ByteBuffer.h"
#include "util/FilePathInfo.h"
#include "util/Hashtable.h"
#include "util/String.h"
#include "util/StringTokenizer.h"

using namespace muscle;

class SymbolRecord
{
public:
   SymbolRecord() : _startAddress(0), _length(0) {/* empty */}

   uint64 _startAddress;
   uint64 _length;
   String _text;
};

class NameAndSymbolRecord : public SymbolRecord
{
public:
   NameAndSymbolRecord() : _name(NULL) {/* empty */}
   NameAndSymbolRecord(const String * name, const SymbolRecord & sr) : SymbolRecord(sr), _name(name) {/* empty */}

   const String * GetName() const {return _name;}

   // Convenience method for easier debugging
   String ToString() const
   {
      char buf[512];
      muscleSprintf(buf, " [" XINT64_FORMAT_SPEC "-" XINT64_FORMAT_SPEC ") (length=" UINT64_FORMAT_SPEC ")", _startAddress, _startAddress+_length, _length);
      return _name->Append(buf); 
   }

private:
   const String * _name;
};

static String CalculateDiffs(const String & textA, const String & textB)
{
   String tempPath;
   if (GetSystemPath(SYSTEM_PATH_TEMPFILES, tempPath) != B_NO_ERROR) return "Error, couldn't find temp folder!";

   const String tmpAPath = tempPath + "executable_diff_temp_a.txt";
   const String tmpBPath = tempPath + "executable_diff_temp_b.txt";

   // Rather than come up with my own diffing algorithm, I'm going to just cheese it and call out to `diff`
   {
      FileDataIO outA(fopen(tmpAPath(), "w"));
      outA.WriteFully(textA(), textA.Length());
   }
   {
      FileDataIO outB(fopen(tmpBPath(), "w"));
      outB.WriteFully(textB(), textB.Length());
   }

   FILE * pIn = popen(String("diff %1 %2").Arg(tmpAPath).Arg(tmpBPath)(), "r");
   if (pIn)
   {
      String ret;
      char buf[2048];
      while(fgets(buf, sizeof(buf), pIn)) ret += buf;
      pclose(pIn);
      return ret;
   }
   else return "Unable to launch diff!";
}

static void PrintSymbolDiffs(const String & symbolName, const String & symbolTextA, const String & symbolTextB, FILE * fpOut)
{
   fprintf(fpOut, "\n\n===================== Diffs for [%s]:\n", symbolName());
   fprintf(fpOut, "%s\n", CalculateDiffs(symbolTextA, symbolTextB)());
}

static bool IsHexChar(char c)
{
   return muscleInRange(c, '0', '9') ||
          muscleInRange(c, 'A', 'F') ||
          muscleInRange(c, 'a', 'f');
}

static String GetSymbolicAddressStringAux(uint64 addr, const Queue<NameAndSymbolRecord> & index, uint32 firstIdx, uint32 afterLastIdx)
{
        if (firstIdx   >= afterLastIdx) return GetEmptyString();
   else if (firstIdx+1 == afterLastIdx)
   {
      // single-object case; either we have it or we don't
      const NameAndSymbolRecord & nasr = index[firstIdx];
      const int64 diff = addr-nasr._startAddress;
      return ((diff >= 0)&&(((uint64)diff < nasr._length))) ? *nasr.GetName() : GetEmptyString();
   }
   else
   {
      // In this case we have at least two NameAndSymbolRecord objects in our range, so we'll divide and recurse
      const uint32 midIdx = (firstIdx+afterLastIdx)/2;
      const NameAndSymbolRecord & midNASR = index[midIdx];
 
      return (addr < midNASR._startAddress) 
         ? GetSymbolicAddressStringAux(addr, index, firstIdx,       midIdx)
         : GetSymbolicAddressStringAux(addr, index,   midIdx, afterLastIdx);
   }
}

/** Returns true iff the 8 bytes pointed to by (s) end in four or more 0-bytes
  * 4-byte integers placed into 8-byte fields seem to occur a lot in the .rodata
  * section; I'm not sure what they are but they appear to be some kind of address
  * offset so I'm going to ignore them as a false-positive.  -jaf
  */
static bool IsOffset(const uint8 * s)
{
   for (int i=4; i<8; i++) if (s[i] != 0) return false;
   return true;
}

static String GetSymbolicAddressString(uint64 addr, const Queue<NameAndSymbolRecord> & index, const ByteBuffer * optROData, uint64 roStart)
{
   // For Linux/objdump:  If addr points to inside the .rodata section, return the literal-string it points to
   if ((optROData)&&(addr >= roStart)&&(addr < (roStart+optROData->GetNumBytes())))
   {
      const uint8 * s = optROData->GetBuffer()+(addr-roStart);
      return IsOffset(s) ? String("{(offset)}") : String("{%1}").Arg(String((const char *) s, (optROData->GetBuffer()+optROData->GetNumBytes())-s));
   }

   return GetSymbolicAddressStringAux(addr, index, 0, index.GetNumItems());
}

static void SanitizeLine(const String & lineStr, String & ret, const Queue<NameAndSymbolRecord> & index, const ByteBuffer * optROData, uint64 roStart)
{
   ret.Clear();

   const char * p = lineStr();
   while(*p)
   {
      if ((p[0] == '-')&&(p[1] == '0')&&(p[2] == 'x')) 
      {
         ret += "-0x";
         p   += 3;  // e.g. for -0x20 we should just skip it, as we can't expand negative addresses
      }
#ifdef __APPLE__
      else if ((p[0] == '0')&&(p[1] == 'x'))
#else
      else if (((p[0] == '0')&&(p[1] == 'x')) || ((p[0] == ' ')&&(IsHexChar(p[1]))))
#endif
      {
         const int offset = ((p[1] == '0')&&(p[2] == 'x')) ? 3 : ((p[1] == 'x') ? 2 : 1);
         const char * q = &p[offset];

         while(IsHexChar(*q)) q++;
         const uint64 addr = Atoxll(&p[offset]);

         const String sas = GetSymbolicAddressString(addr, index, optROData, roStart);
         if (sas.HasChars())
         {
            ret += sas; // insert our expanded (symbol-relative) representation
            p = q;
         }
         else ret += *p++;  // lookup failed?  Then leave it as-is (it's probably a numeric constant)
      }
      else ret += *p++;
   }

   ret.Replace("\n", "\\n");  // newlines mess with the diff output, otherwise

   if (ret.EndsWith('>'))
   {
      // Neutralize any address indicators like "<main+0x9b6>" down to just "<main>"
      const int32 ob = ret.LastIndexOf('<');
      const int32 pb = (ob >= 0) ? ret.LastIndexOf('+') : -1;
      if (pb > ob) ret = ret.Substring(0, pb) + ">";  // remove the "+09b6" part
   }
}

// Replaces any obvious addresses with a fixed dummy-string, to avoid false-positive diffs
static void SanitizeAddresses(String & text, const Queue<NameAndSymbolRecord> & index, const ByteBuffer * optROData, uint64 roStart)
{
   String outStr;

   String scratchStr;
   StringTokenizer tok(text(), "\n");
   const char * t;
   while((t=tok()) != NULL)
   {
      SanitizeLine(t, scratchStr, index, optROData, roStart);
      outStr += scratchStr;
      outStr += '\n';
   }

   text = outStr;
}

static uint32 GetHexLength(const char * p)
{
   uint32 count = 0;
   while(IsHexChar(*p)) {count++; p++;}
   return count;
}

static bool IsPointerOrOffset(const char * s)
{
   const uint32 hexLength = GetHexLength(s);
   return ((hexLength >= 4)||(strncmp(&s[hexLength], "(%r", 3) == 0));
}

// Replaces any obvious addresses with a fixed dummy-string, to avoid false-positive diffs
static String GetWithNeutralizedAddresses(const String & s)
{
   // Find any hex values starting with 0x, and replace the hex-number with "?", to avoid pointless diffs
   const char * x = strstr(s(), "0x");
   if (x)
   {
      char outBuf[2048];

      const char * in = s();
      char * out = outBuf;

      bool inHex = false;
      while(*in)
      {
         if (inHex)
         {
            inHex = IsHexChar(*in);
            if (inHex == false) *out++ = *in;
         }
#ifdef __APPLE__
         else if ((in[0] == '0')&&(in[1] == 'x')&&(IsPointerOrOffset(&in[2])))
#else
         else if (((in[0] == '0')&&(in[1] == 'x')&&(IsPointerOrOffset(&in[2]))) ||
                  ((in[0] == '#')&&(in[1] == ' ')&&(IsPointerOrOffset(&in[2]))))
#endif
         {
            inHex = true;
            *out++ = '0';
            *out++ = 'x';
            *out++ = '?';
            in++;  // pass the '0'
         }
         else *out++ = *in;

         in++;
      }
      *out = '\0';

      return outBuf;
   }
   else return s;
}

static String GetUniqueSymbolName(const String & symbolName, const Hashtable<String, SymbolRecord> & symbols)
{
   String s = symbolName + "#0";
   while(symbols.ContainsKey(s))
   {
      uint32 suffix = 0;
      s = s.WithoutNumericSuffix(&suffix);
      s += String("%1").Arg(suffix+1);
   }
   return s;
}

class CompareStartAddressesFunctor 
{
public:
   CompareStartAddressesFunctor() {/* empty */}

   int Compare(const SymbolRecord & r1, const SymbolRecord & r2, void *) const
   {
      const int addrDiff = muscleCompare(r1._startAddress, r2._startAddress);
      return addrDiff ? addrDiff : muscleCompare(r1._text, r2._text);  // paranoia
   }
};

static void PrintSanitizerStatus(uint32 count, uint32 total)
{
   LogTime(MUSCLE_LOG_INFO, "Reconstructing symbol addresses: " UINT32_FORMAT_SPEC "/" UINT32_FORMAT_SPEC " (%.0f%%)...\r", count, total, (100.0f*count)/total);
}

#ifdef __APPLE__

// Routine for parsing the output of Apple's otool disassembler utility
static Hashtable<String, SymbolRecord> ParseOtoolOutput(const char * fileName)
{
   const char * otoolPath = "/usr/bin/otool";

   const FilePathInfo fpi(otoolPath);
   if (fpi.Exists() == false)
   {
      String toolName = otoolPath;
      toolName = toolName.Substring("/");  // only include the executable name itself
      LogTime(MUSCLE_LOG_CRITICALERROR, "%s not found -- executable_diff needs to be able run %s in order to function.\n", otoolPath, toolName());
      LogTime(MUSCLE_LOG_CRITICALERROR, "To install %s, install XCode (and its command line tools)\n", otoolPath, toolName(), toolName());
      exit(10);
   }

   FILE * fpIn = popen(String("%1 -tV '%2'").Arg(otoolPath).Arg(fileName)(), "r");
   if (fpIn == NULL)
   {
      LogTime(MUSCLE_LOG_CRITICALERROR, "Unable to open executable [%s] for reading\n", fileName);
      exit(10);
   }

   LogTime(MUSCLE_LOG_INFO, "Opening executable file [%s]...\n", fileName);
   Hashtable<String, SymbolRecord> symbols;
   (void) symbols.EnsureSize(100000);  // try to avoid reallocations as they could be expensive

   SymbolRecord * curSymbolContents  = NULL;

   uint32 lineNumber  = 1;
   uint32 numSymbols  = 0;
   uint64 lastPrintAt = GetRunTime64();

   const String ripStr = "(%rip)";  // indicates instruction-pointer-relative addressing!
   const String rspStr = "(%rip)";  // stack-frame-pointer

   char buf[2048];
   (void) fgets(buf, sizeof(buf), fpIn);   // skip the first line, as it is just the name of the executable
   while(fgets(buf, sizeof(buf), fpIn))
   {
      String line = buf;
      line = line.Trim();

      if (line.EndsWith(':'))
      {
         line--;
         curSymbolContents = symbols.PutAndGet(GetUniqueSymbolName(line, symbols));
         if (curSymbolContents) numSymbols++;
                           else WARN_OUT_OF_MEMORY;
      }
      else if (curSymbolContents)
      {
         const int32 firstTabIdx = line.IndexOf('\t');
         if (firstTabIdx >= 0) 
         {
            const uint64 addr = Atoxll(line.Substring(0, firstTabIdx).Trim()());
            if (curSymbolContents->_startAddress == 0) curSymbolContents->_startAddress = addr;
            curSymbolContents->_length = muscleMax(curSymbolContents->_length, (addr-curSymbolContents->_startAddress)+4);
            line = line.Substring(firstTabIdx+1);
         }

         String & text = curSymbolContents->_text;

         const int32 commentStartIdx = line.IndexOf(" ## ");
         const String beforeComment  = (commentStartIdx>=0) ? line.Substring(0, commentStartIdx) : line;
         const String comment        = (commentStartIdx>=0) ? line.Substring(commentStartIdx)    : GetEmptyString();
         const bool neutralize = ((line.Contains("(%rip)"))||(comment.Contains(" for: "))||(comment.Contains(" symbol address:"))) || (((line.StartsWith("call"))||(line.StartsWith("jmp")))&&(commentStartIdx < 0));
         const bool keepComment = comment.Contains("literal");

         String l = neutralize ? GetWithNeutralizedAddresses(beforeComment) : beforeComment;
         if (keepComment) l += comment;

         text += l;
         text += '\n';
      }

      if (OnceEvery(MillisToMicros(100), lastPrintAt)) LogTime(MUSCLE_LOG_INFO, "Parsing otool output: " UINT32_FORMAT_SPEC " lines (" UINT32_FORMAT_SPEC " symbols) ...\r", lineNumber, numSymbols);
      lineNumber++;
   }
   pclose(fpIn);

   symbols.SortByValue(CompareStartAddressesFunctor());

   Queue<NameAndSymbolRecord> index;  // used for quick (O(logN)) address-lookups
   (void) index.EnsureSize(symbols.GetNumItems());
   for (HashtableIterator<String, SymbolRecord> iter(symbols); iter.HasData(); iter++) (void) index.AddTail(NameAndSymbolRecord(&iter.GetKey(), iter.GetValue()));

   // Now go through and replace any absolute addresses with symbol-relative representations
   // (necessary since the addresses of the symbols may be different)
   uint32 count = 0;
   for (HashtableIterator<String, SymbolRecord> iter(symbols); iter.HasData(); iter++,count++)
   {
      SanitizeAddresses(iter.GetValue()._text, index, NULL, 0);
      if (OnceEvery(MillisToMicros(100), lastPrintAt)) PrintSanitizerStatus(count, symbols.GetNumItems());
   }
   PrintSanitizerStatus(count, symbols.GetNumItems());
   printf("\n");

   symbols.SortByKey();
   LogTime(MUSCLE_LOG_INFO, "Parsed " UINT32_FORMAT_SPEC " unique symbols from %s\n", symbols.GetNumItems(), fileName);
   return symbols;
}

#else

// Routine for parsing the output of Linux's objdump disassembler utility
static Hashtable<String, SymbolRecord> ParseObjdumpOutput(const char * fileName)
{
   const char * otoolPath = "/usr/bin/objdump";

   const FilePathInfo fpi(otoolPath);
   if (fpi.Exists() == false)
   {
      String toolName = otoolPath;
      toolName = toolName.Substring("/");  // only include the executable name itself
      LogTime(MUSCLE_LOG_CRITICALERROR, "%s not found -- executable_diff needs to be able run %s in order to function.\n", otoolPath, toolName());
      exit(10);
   }

   FILE * fpIn = popen(String("%1 -d --no-show-raw-insn '%2'").Arg(otoolPath).Arg(fileName)(), "r");
   if (fpIn == NULL)
   {
      LogTime(MUSCLE_LOG_CRITICALERROR, "Unable to open executable [%s] for reading\n", fileName);
      exit(10);
   }

   LogTime(MUSCLE_LOG_INFO, "Opening executable file [%s]...\n", fileName);
   Hashtable<String, SymbolRecord> symbols;
   (void) symbols.EnsureSize(100000);  // try to avoid reallocations as they could be expensive

   SymbolRecord * curSymbolContents  = NULL;

   uint32 lineNumber  = 1;
   uint32 numSymbols  = 0;
   uint64 lastPrintAt = GetRunTime64();

   char buf[2048];
   (void) fgets(buf, sizeof(buf), fpIn);   // skip the first line, as it is just the name of the executable
   while(fgets(buf, sizeof(buf), fpIn))
   {
      String line = buf;
      line = line.Trim();

      if (line.EndsWith(">:"))
      {
         const uint64 addr = Atoxll(line());
         if (addr == 0) continue;

         if (curSymbolContents)
         {
            curSymbolContents->_length = muscleMax(curSymbolContents->_length, (addr-curSymbolContents->_startAddress));
            curSymbolContents = NULL;
         }

         curSymbolContents = symbols.PutAndGet(GetUniqueSymbolName(line.Substring("<").Substring(0,">"), symbols));
         if (curSymbolContents)
         {
             curSymbolContents->_startAddress = addr;
             numSymbols++;
         }
         else WARN_OUT_OF_MEMORY;
      }
      else if (curSymbolContents)
      {
         const int32 firstTabIdx = line.IndexOf('\t');
         if (firstTabIdx >= 0) line = line.Substring(firstTabIdx+1).Trim();  // skip past the address-column (e.g. "  4137ac:\t")

         const bool neutralize = (line.Contains("%rip"))||(line.Contains("%rsp"))||(line.EndsWith('>'))||((line.StartsWith("call"))||(line.StartsWith("jmp")));
         String & text = curSymbolContents->_text;
         text += neutralize ? GetWithNeutralizedAddresses(line) : line;
         text += '\n';
      }

      if (OnceEvery(MillisToMicros(100), lastPrintAt)) LogTime(MUSCLE_LOG_INFO, "Parsing objdump output: " UINT32_FORMAT_SPEC " lines (" UINT32_FORMAT_SPEC " symbols) ...\r", lineNumber, numSymbols);
      lineNumber++;
   }
   pclose(fpIn);

   symbols.SortByValue(CompareStartAddressesFunctor());

   Queue<NameAndSymbolRecord> index;  // used for quick (O(logN)) address-lookups
   (void) index.EnsureSize(symbols.GetNumItems());
   for (HashtableIterator<String, SymbolRecord> iter(symbols); iter.HasData(); iter++) (void) index.AddTail(NameAndSymbolRecord(&iter.GetKey(), iter.GetValue()));

   // For Linux, we'll also need to parse out the .rodata section by hand,
   // since objdump doesn't have the helpful literal-annotations that otool has
   uint64 roStart = 0;
   ByteBufferRef roData = GetByteBufferFromPool(0);
   if (roData())
   {
      FILE * fpIn = popen(String("%1 -sj .rodata '%2'").Arg(otoolPath).Arg(fileName)(), "r");
      if (fpIn)
      {
         String scratchHexStr;

         bool parse = false;
         while(fgets(buf, sizeof(buf), fpIn))
         {
            if (parse)
            {
               String b = buf; b = b.Trim();
               StringTokenizer tok(b(), " ");
               const char * addr = tok();
               if (roStart == 0) roStart = Atoxll(addr);

               scratchHexStr.Clear();
               for (uint32 i=0; i<4; i++) scratchHexStr += tok();

               for (uint32 i=0; (i+1)<scratchHexStr.Length(); i+=2)
               {
                  char tempBuf[3];
                  memcpy(tempBuf, scratchHexStr()+i, 2);
                  tempBuf[2] = '\0';
                  (void) roData()->AppendByte((uint8) strtol(tempBuf, NULL, 16));
               }
            } 
            else if (strncmp(buf, "Contents of section .rodata:", 28) == 0) parse = true;
         }

         pclose(fpIn);
      }
   }
   
   // Now go through and replace any absolute addresses with symbol-relative representations
   // (necessary since the addresses of the symbols may be different)
   uint32 count = 0;
   for (HashtableIterator<String, SymbolRecord> iter(symbols); iter.HasData(); iter++,count++)
   {
      SanitizeAddresses(iter.GetValue()._text, index, roData(), roStart);
      if (OnceEvery(MillisToMicros(100), lastPrintAt)) PrintSanitizerStatus(count, symbols.GetNumItems());
   }
   PrintSanitizerStatus(count, symbols.GetNumItems());
   printf("\n");

   symbols.SortByKey();
   LogTime(MUSCLE_LOG_INFO, "Parsed " UINT32_FORMAT_SPEC " unique symbols from %s\n", symbols.GetNumItems(), fileName);

   return symbols;
}
#endif

static Hashtable<String, SymbolRecord> ParseExecutableFile(const char * fileName)
{
#ifdef __APPLE__
   return ParseOtoolOutput(fileName);
#else
   return ParseObjdumpOutput(fileName);
#endif
}

static uint32 RemoveMatchingSymbolsAux(Hashtable<String, SymbolRecord> & tableA, Hashtable<String, SymbolRecord> & tableB)
{
   uint32 ret = 0;
   for (HashtableIterator<String, SymbolRecord> iter(tableA); iter.HasData(); iter++)
   {
      const String & symbolName = iter.GetKey();
      const SymbolRecord & valA = iter.GetValue();
      const SymbolRecord * valB = tableB.Get(symbolName);
      if ((valB)&&(valB->_text == valA._text))
      {
         (void) tableB.Remove(symbolName);
         (void) tableA.Remove(symbolName);  // gotta do this last, as it invalidates (symbolName)
         ret++;
      }
   }
   return ret;
}

static void ReportDifferingSymbolsAux(const char * fileA, const Hashtable<String, SymbolRecord> & tableA, const char * fileB, const Hashtable<String, SymbolRecord> & tableB, Hashtable<String, Void> & reported, FILE * fpOut)
{
   for (HashtableIterator<String, SymbolRecord> iter(tableA); iter.HasData(); iter++)
   {
      const String & symbolName = iter.GetKey();
      if (reported.ContainsKey(symbolName) == false)
      {
         (void) reported.PutWithDefault(symbolName);

         const SymbolRecord & valA = iter.GetValue();
         const SymbolRecord * valB = tableB.Get(symbolName);
         if (valB) 
         {
            LogTime(MUSCLE_LOG_WARNING, "Diffs detected in symbol [%s]\n", symbolName());
            if (fpOut) PrintSymbolDiffs(symbolName, valA._text, valB->_text, fpOut);
         }
         else LogTime(MUSCLE_LOG_WARNING, "Symbol [%s] exists in [%s] but is not present in [%s]\n", symbolName(), fileA, fileB);
      }
   }
}

int main(int argc, char ** argv) 
{
   CompleteSetupSystem css;

   if (argc < 3)
   {
      LogTime(MUSCLE_LOG_CRITICALERROR, "Usage:  ./executable_diff ./CueStationA.app/Contents/MacOS/CueStation ./CueStationB.app/Contents/MacOS/CueStation\n");
      return 10;
   }

   const char * fileA = argv[1];
   const char * fileB = argv[2];

   printf("\n");
   Hashtable<String, SymbolRecord> tableA = ParseExecutableFile(fileA);

   printf("\n");
   Hashtable<String, SymbolRecord> tableB = ParseExecutableFile(fileB);

   printf("\n");

   // Get rid of everything that didn't change, we're not interested in that
   uint32 numRemoved = 0;
   numRemoved += RemoveMatchingSymbolsAux(tableA, tableB);
   numRemoved += RemoveMatchingSymbolsAux(tableB, tableA);

   printf("\n");
   printf("-------------------------------------------------------------\n");
   printf("\n");

   LogTime(MUSCLE_LOG_INFO, "Found " UINT32_FORMAT_SPEC " matching symbols and " UINT32_FORMAT_SPEC " non-matching symbols.\n", numRemoved, tableA.GetNumItems(), tableB.GetNumItems());

   String reportFileName = String("executable_diffs_report_") + GetHumanReadableTimeString(GetCurrentTime64()) + ".txt";
   reportFileName.Replace('/', '_');
   reportFileName.Replace(':', '_');
   reportFileName.Replace(' ', '_');
   FILE * fpOut = fopen(reportFileName(), "w");
   
   Hashtable<String, Void> reported;
   ReportDifferingSymbolsAux(fileA, tableA, fileB, tableB, reported, fpOut);
   ReportDifferingSymbolsAux(fileB, tableB, fileA, tableA, reported, fpOut);

   if (fpOut)
   {
      LogTime(MUSCLE_LOG_INFO, "Diffs report written to file [%s]\n", reportFileName());
      fclose(fpOut);
   }

   return 0;
}
