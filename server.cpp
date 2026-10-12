// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
//#include <unistd.h>
//#include <sys/socket.h>
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2;
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024;
const int32_t IO_BUFFER_SIZE = 64 * 1024;
const int32_t SOCKET_TIMEOUT_SEC = 5;

// ---- Core Structs ----
struct Variable {
    string name;
    int32_t value;
};

struct Frame {
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};

struct Snapshot {
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};

// ---- Manual Helper Functions ----
int32_t parseNumber(const string& s) {
    int32_t val = 0;
    int32_t start = 0;
    int32_t sign = 1;
    if (s.length() > 0 && s[0] == '-') {
        sign = -1;
        start = 1;
    }
    for (int i = start; i < s.length(); i++) {
        val = val * 10 + (s[i] - '0');
    }
    return val * sign;
}

string intToString(int32_t num) {
    if (num == 0) return "0";
    string res = "";
    while (num > 0) {
        res = (char)('0' + (num % 10)) + res;
        num /= 10;
    }
    return res;
}

int32_t getVarValue(Frame& frame, const string& name) {
    for (int i = 0; i < frame.localCount; i++) {
        if (frame.locals[i].name == name) return frame.locals[i].value;
    }
    for (int i = 0; i < frame.argc; i++) {
        if (frame.argv[i].name == name) return frame.argv[i].value;
    }
    return parseNumber(name);
}

void setVarValue(Frame& frame, const string& name, int32_t val) {
    for (int i = 0; i < frame.localCount; i++) {
        if (frame.locals[i].name == name) {
            frame.locals[i].value = val;
            return;
        }
    }
    for (int i = 0; i < frame.argc; i++) {
        if (frame.argv[i].name == name) {
            frame.argv[i].value = val;
            return;
        }
    }
    frame.locals[frame.localCount].name = name;
    frame.locals[frame.localCount].value = val;
    frame.localCount++;
}

// ---- Custom Data Structures ----
template <typename T>
class Stack {
    struct Node {
        T data;
        Node* next;
    };
    Node* top;
    int32_t count;

public:
    Stack() {
        top = nullptr;
        count = 0;
    }
    void push(const T& val) {
        if (count < MAX_STACK_DEPTH) {
            Node* newNode = new Node();
            newNode->data = val;
            newNode->next = top;
            top = newNode;
            count++;
        }
    }
    T pop() {
        T val;
        if (top != nullptr) {
            Node* temp = top;
            val = top->data;
            top = top->next;
            delete temp;
            count--;
        }
        return val;
    }
    T& peek() {
        return top->data;
    }
    bool isEmpty() {
        return top == nullptr;
    }
    int32_t depth() {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen) {
        int32_t written = 0;
        Node* curr = top;
        while (curr != nullptr && written < maxLen) {
            out[written] = curr->data;
            written++;
            curr = curr->next;
        }
        return written;
    }
};

struct TimelineNode {
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;
};

class Timeline {
    TimelineNode* head, * tail;
    int32_t stepCount;

public:
    Timeline() {
        head = nullptr;
        tail = nullptr;
        stepCount = 0;
    }
    void record(Snapshot* s) {
        TimelineNode* newNode = new TimelineNode();
        newNode->data = s;
        newNode->next = nullptr;
        newNode->prev = tail;
        if (tail == nullptr) {
            head = newNode;
            tail = newNode;
        }
        else {
            tail->next = newNode;
            tail = newNode;
        }
        stepCount++;
    }
    TimelineNode* begin() {
        return head;
    }
    int32_t getStepCount() {
        return stepCount;
    }
};

struct TTDBHeader {
    char magic[4];
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

// resolve.bin - bookkeeping
struct FuncEntry {
    string funcName;
    int64_t byteOffsetInResolveBin;
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
        if (hasChars) return true;
    }
    return false;
}

string firstWord(const string& line) {
    string word = "";
    int i = 0;
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
    if (fread(&offsetField, sizeof(int64_t), 1, f) != 1) return -1;
    int32_t size;
    fread(&size, sizeof(int32_t), 1, f);
    char buffer[2048];
    fread(buffer, 1, size, f);
    outText = string(buffer, size);
    return offsetField;
}

int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath) {
    ifstream in(sourcePath);
    if (!in.is_open()) {
        cout << "Error: Could not open " << sourcePath << endl;
        return -1;
    }
    FILE* out = fopen(resolveBinPath, "wb");
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

// PASS 0x2: EXECUTION 
enum TokenType { KEYWORD, IDENTIFIER, PARAM };
struct Token {
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
                    if (tokenCount == 0) tokens[tokenCount].type = KEYWORD;
                    else if (tokenCount == 1) tokens[tokenCount].type = IDENTIFIER;
                    else tokens[tokenCount].type = PARAM;

                    tokens[tokenCount].text = currentWord;
                    tokenCount++;
                }
                currentWord = "";
            }
        }
        else currentWord += line[i];
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
    if (!f) return;
    FuncEntry funcTable[MAX_FUNCS];
    int32_t funcCount = 0;
    string line;
    fseek(f, 0, SEEK_SET);
    while (true) {
        int64_t currentOffset = readResolveRecord(f, line);
        if (currentOffset == -1) break;
        Token tempTokens[MAX_TOKENS];
        int32_t count = tokenizeLine(line, tempTokens, MAX_TOKENS);
        if (count > 0 && tempTokens[0].text == "func") {
            funcTable[funcCount].funcName = tempTokens[1].text;
            funcTable[funcCount].byteOffsetInResolveBin = currentOffset;
            funcCount++;
        }
    }
    Stack<Frame> callStack;
    Frame mainFrame;
    mainFrame.func_name = "main";
    mainFrame.argc = 0;
    mainFrame.localCount = 0;
    mainFrame.returnLine = -1;
    callStack.push(mainFrame);
    fseek(f, mainOffset, SEEK_SET);
    while (!callStack.isEmpty()) {
        int64_t currentOffset = readResolveRecord(f, line);
        if (currentOffset == -1) break;
        Token tokens[MAX_TOKENS];
        int32_t count = tokenizeLine(line, tokens, MAX_TOKENS);
        if (count > 0) {
            string kw = tokens[0].text;
            if (kw == "func_end") {
                callStack.pop();
                if (!callStack.isEmpty()) fseek(f, callStack.peek().returnLine, SEEK_SET);
            }
            else if (kw == "set") {
                int32_t val = getVarValue(callStack.peek(), tokens[2].text);
                setVarValue(callStack.peek(), tokens[1].text, val);
            }
            else if (kw == "add") {
                int32_t val1 = getVarValue(callStack.peek(), tokens[1].text);
                int32_t val2 = getVarValue(callStack.peek(), tokens[2].text);
                setVarValue(callStack.peek(), tokens[1].text, val1 + val2);
            }
            else if (kw == "sub") {
                int32_t val1 = getVarValue(callStack.peek(), tokens[1].text);
                int32_t val2 = getVarValue(callStack.peek(), tokens[2].text);
                setVarValue(callStack.peek(), tokens[1].text, val1 - val2);
            }
            else if (kw == "call") {
                string targetFunc = tokens[1].text;
                int64_t targetOffset = -1;
                for (int i = 0; i < funcCount; i++) {
                    if (funcTable[i].funcName == targetFunc) {
                        targetOffset = funcTable[i].byteOffsetInResolveBin;
                        break;
                    }
                }
                if (targetOffset != -1) {
                    callStack.peek().returnLine = ftell(f);
                    Frame newFrame;
                    newFrame.func_name = targetFunc;
                    newFrame.argc = count - 2;
                    newFrame.localCount = 0;
                    newFrame.returnLine = -1;
                    for (int i = 0; i < newFrame.argc; i++) {
                        newFrame.argv[i].name = "arg" + intToString(i);
                        newFrame.argv[i].value = getVarValue(callStack.peek(), tokens[i + 2].text);
                    }
                    callStack.push(newFrame);
                    fseek(f, targetOffset, SEEK_SET);
                }
            }
            timeline.record(buildSnapshot(callStack));
        }
    }
    fclose(f);
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline& timeline, const char* tdbgPath) {
    FILE* f = fopen(tdbgPath, "wb");
    if (!f) return;
    TTDBHeader header;
    header.magic[0] = 'T';
    header.magic[1] = 'T';
    header.magic[2] = 'D';
    header.magic[3] = 'B';
    header.version = 1;
    header.stepCount = timeline.getStepCount();
    header.indexOffset = 0;
    fwrite(&header, sizeof(TTDBHeader), 1, f);
    int64_t* indexArray = new int64_t[header.stepCount];
    int32_t currentIndex = 0;
    TimelineNode* curr = timeline.begin();
    while (curr != nullptr) {
        indexArray[currentIndex] = ftell(f);
        fwrite(curr->data, sizeof(Snapshot), 1, f);
        curr = curr->next;
        currentIndex++;
    }
    header.indexOffset = ftell(f);
    fwrite(indexArray, sizeof(int64_t), header.stepCount, f);
    fseek(f, 0, SEEK_SET);
    fwrite(&header, sizeof(TTDBHeader), 1, f);
    fclose(f);
    delete[] indexArray;
}

// ---- MAIN ----
int32_t main() {
    if (!validateProgram("source.bin")) {
        return 1;
    }
    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");
    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);
    writeTdbg(timeline, "session.tdbg");
    return 0;
}