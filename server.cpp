// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
//#include <unistd.h>
//#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *top;
    int32_t count;

public:
    // Implement these functions:
    Stack()
    { // initialize the stack
    }
    void push(const T &val)
    {

        // pushes the value on the stack if max limit is not reached yet.
    }
    T pop()
    {
        // pop the top value on the stack
    }
    T &peek()
    {
        // returns the top value on the stack
    }
    bool isEmpty()
    {
    }
    int32_t depth()
    {
        return 0;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        return 0;
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot *data;
    TimelineNode *next;
    TimelineNode *prev;
};
class Timeline
{
    TimelineNode *head, *tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
    }
    void record(Snapshot *s)
    {
        // add record in the timeline
    }
    TimelineNode *begin()
    {
    }
    int32_t getStepCount()
    {
        return 0;
    }
};

// Core structs
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE *f, const TTDBHeader &h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};



// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream& in, string& out) {
    while (getline(in, out)) {
        bool hasChars = false;
        for (int i = 0; i < out.length(); i++) {
            if (out[i] != ' ' && out[i] != '\t' && out[i] != '\r') {
                hasChars = true;
                break;
            }
        }
        if (hasChars) {
            return true;
        }
    }
    return false;
}

string firstWord(const string& line) {
    string word = "";
    int i = 0;
    while (i < line.length() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) {
        i++;
    }
    while (i < line.length() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') {
        word += line[i];
        i++;
    }
    return word;
}

string secondWord(const string& line) {
    string word = "";
    int i = 0;
    while (i < line.length() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) i++;
    while (i < line.length() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') i++;
    while (i < line.length() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) i++;
    while (i < line.length() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') {
        word += line[i];
        i++;
    }
    return word;
}

bool validateProgram(const char* sourcePath) {
    ifstream in(sourcePath);
    if (!in.is_open()) {
        cout << "Error: Could not open " << sourcePath << endl;
        return false;
    }
    string line;
    bool insideFunc = false;
    while (readSourceLine(in, line)) {
        string fw = firstWord(line);
        if (fw == "func") {
            if (insideFunc == true) {
                cout << "Validation Error: Nested functions are not allowed." << endl;
                return false;
            }
            insideFunc = true;
        }
        else if (fw == "func_end") {
            if (insideFunc == false) {
                cout << "Validation Error: func_end found without a matching func." << endl;
                return false;
            }
            insideFunc = false;
        }
    }
    if (insideFunc == true) {
        cout << "Validation Error: Missing func_end at the end of the file." << endl;
        return false;
    }
    cout << "Validation Successful: Structural integrity verified." << endl;
    return true;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text) {
    int64_t startPos = ftell(f);
    int32_t size = text.length();
    fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&size, sizeof(int32_t), 1, f);
    fwrite(text.c_str(), 1, size, f);
    return startPos;
}

int64_t readResolveRecord(FILE* f, string& outText) {
    int64_t offsetField;
    if (fread(&offsetField, sizeof(int64_t), 1, f) != 1) {
        return -1;
    }
    int32_t size;
    fread(&size, sizeof(int32_t), 1, f);
    char buffer[2048];
    fread(buffer, 1, size, f);
    outText = string(buffer, size);
    return offsetField;
}
int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath) {
    ifstream in(sourcePath);
    if (!in.is_open()) {
        cout << "Error: Could not open " << sourcePath << endl;
        return -1;
    }
    FILE *out = fopen(resolveBinPath, "wb");
    if (!out) {
        cout << "Error: Could not create " << resolveBinPath << endl;
        return -1;
    }
    string line;
    int64_t defaultOffset = 0;
    while (readSourceLine(in, line)) {
        writeResolveRecord(out, defaultOffset, line);
    }
    fclose(out);
    cout << "Resolve Successful: Created resolve.bin." << endl;
    return 0;
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens) {
    int32_t tokenCount = 0;
    string currentWord = "";
    for (int i = 0; i <= line.length(); i++) {
        if (i == line.length() || line[i] == ' ' || line[i] == '\t' || line[i] == '\r') {
            if (currentWord.length() > 0) {
                if (tokenCount < maxTokens) {
                    if (tokenCount == 0) {
                        tokens[tokenCount].type = KEYWORD;
                    }
                    else if (tokenCount == 1) {
                        tokens[tokenCount].type = IDENTIFIER;
                    }
                    else {
                        tokens[tokenCount].type = PARAM;
                    }
                    tokens[tokenCount].text = currentWord;
                    tokenCount++;
                }
                currentWord = "";
            }
        }
        else {
            currentWord += line[i];
        }
    }
    return tokenCount;
}

Snapshot* buildSnapshot(Stack<Frame>& callStack) {
    Snapshot* snap = new Snapshot();
    snap->stackDepth = callStack.snapshot_into(snap->callStack, MAX_STACK_DEPTH);
    return snap;
}
void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline) {
    FILE* f = fopen(resolveBinPath, "rb");
    if (!f) {
        cout << "Error opening " << resolveBinPath << endl;
        return;
    }
    Stack<Frame> callStack;
    Frame mainFrame;
    mainFrame.func_name = "main";
    mainFrame.argc = 0;
    mainFrame.localCount = 0;
    mainFrame.returnLine = -1;
    callStack.push(mainFrame);
    fseek(f, mainOffset, SEEK_SET);
    string line;
    while (!callStack.isEmpty()) {
        int64_t currentOffset = readResolveRecord(f, line);
        if (currentOffset == -1) {
            break;
        }
        Token tokens[MAX_TOKENS];
        int32_t count = tokenizeLine(line, tokens, MAX_TOKENS);
        if (count > 0) {
            string kw = tokens[0].text;
            if (kw == "func_end") {
                callStack.pop();
                if (!callStack.isEmpty()) {
                    fseek(f, callStack.peek().returnLine, SEEK_SET);
                }
            }
            timeline.record(buildSnapshot(callStack));
        }
    }
    fclose(f);
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section
int32_t main()
{
    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }
    
    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}