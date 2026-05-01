const std = @import("std");
const builtin = @import("builtin");

const TokenKind = enum {
    eof,
    ident,
    string,
    at,
    lparen,
    rparen,
    lbrace,
    rbrace,
    comma,
    semicolon,
    eq,
    kw_string,
    kw_task,
    kw_if,
    kw_else,
};

const Token = struct {
    kind: TokenKind,
    lexeme: []const u8,
    str_value: ?[]const u8 = null,
    line: usize,
    col: usize,
};

const Tokenizer = struct {
    allocator: std.mem.Allocator,
    src: []const u8,
    idx: usize = 0,
    line: usize = 1,
    col: usize = 1,

    fn init(allocator: std.mem.Allocator, src: []const u8) Tokenizer {
        return .{ .allocator = allocator, .src = src };
    }

    fn peek(self: *Tokenizer) u8 {
        return if (self.idx < self.src.len) self.src[self.idx] else 0;
    }

    fn advance(self: *Tokenizer) u8 {
        const c = self.peek();
        if (c == 0) return 0;
        self.idx += 1;
        if (c == '\n') {
            self.line += 1;
            self.col = 1;
        } else {
            self.col += 1;
        }
        return c;
    }

    fn skipWhitespace(self: *Tokenizer) void {
        while (true) {
            const c = self.peek();
            if (c == 0) return;
            if (c == ' ' or c == '\t' or c == '\r' or c == '\n') {
                _ = self.advance();
                continue;
            }
            if (c == '/' and self.idx + 1 < self.src.len and self.src[self.idx + 1] == '/') {
                _ = self.advance();
                _ = self.advance();
                while (self.peek() != 0 and self.peek() != '\n') {
                    _ = self.advance();
                }
                continue;
            }
            return;
        }
    }

    fn next(self: *Tokenizer) !Token {
        self.skipWhitespace();
        const start = self.idx;
        const line = self.line;
        const col = self.col;
        const c = self.peek();
        if (c == 0) {
            return .{ .kind = .eof, .lexeme = "", .line = line, .col = col };
        }

        switch (c) {
            '@' => {
                _ = self.advance();
                return .{ .kind = .at, .lexeme = "@", .line = line, .col = col };
            },
            '(' => {
                _ = self.advance();
                return .{ .kind = .lparen, .lexeme = "(", .line = line, .col = col };
            },
            ')' => {
                _ = self.advance();
                return .{ .kind = .rparen, .lexeme = ")", .line = line, .col = col };
            },
            '{' => {
                _ = self.advance();
                return .{ .kind = .lbrace, .lexeme = "{", .line = line, .col = col };
            },
            '}' => {
                _ = self.advance();
                return .{ .kind = .rbrace, .lexeme = "}", .line = line, .col = col };
            },
            ',' => {
                _ = self.advance();
                return .{ .kind = .comma, .lexeme = ",", .line = line, .col = col };
            },
            ';' => {
                _ = self.advance();
                return .{ .kind = .semicolon, .lexeme = ";", .line = line, .col = col };
            },
            '=' => {
                _ = self.advance();
                return .{ .kind = .eq, .lexeme = "=", .line = line, .col = col };
            },
            '"' => {
                _ = self.advance();
                var buf = try std.ArrayList(u8).initCapacity(self.allocator, 0);
                while (true) {
                    const ch = self.peek();
                    if (ch == 0) return error.UnterminatedString;
                    if (ch == '"') {
                        _ = self.advance();
                        break;
                    }
                    if (ch == '\\') {
                        _ = self.advance();
                        const esc = self.peek();
                        if (esc == 0) return error.UnterminatedString;
                        _ = self.advance();
                        switch (esc) {
                            'n' => try buf.append(self.allocator, '\n'),
                            't' => try buf.append(self.allocator, '\t'),
                            'r' => try buf.append(self.allocator, '\r'),
                            '"' => try buf.append(self.allocator, '"'),
                            '\\' => try buf.append(self.allocator, '\\'),
                            else => return error.InvalidEscape,
                        }
                    } else {
                        _ = self.advance();
                        try buf.append(self.allocator, ch);
                    }
                }
                const s = try buf.toOwnedSlice(self.allocator);
                return .{ .kind = .string, .lexeme = self.src[start..self.idx], .str_value = s, .line = line, .col = col };
            },
            else => {},
        }

        if (isIdentStart(c)) {
            _ = self.advance();
            while (isIdentContinue(self.peek())) {
                _ = self.advance();
            }
            const lex = self.src[start..self.idx];
            if (std.mem.eql(u8, lex, "string")) return .{ .kind = .kw_string, .lexeme = lex, .line = line, .col = col };
            if (std.mem.eql(u8, lex, "task")) return .{ .kind = .kw_task, .lexeme = lex, .line = line, .col = col };
            if (std.mem.eql(u8, lex, "if")) return .{ .kind = .kw_if, .lexeme = lex, .line = line, .col = col };
            if (std.mem.eql(u8, lex, "else")) return .{ .kind = .kw_else, .lexeme = lex, .line = line, .col = col };
            return .{ .kind = .ident, .lexeme = lex, .line = line, .col = col };
        }

        return error.UnexpectedCharacter;
    }
};

fn isIdentStart(c: u8) bool {
    return (c >= 'a' and c <= 'z') or (c >= 'A' and c <= 'Z') or c == '_';
}

fn isIdentContinue(c: u8) bool {
    return isIdentStart(c) or (c >= '0' and c <= '9');
}

const Expr = union(enum) {
    str: []const u8,
    var_ref: []const u8,
    builtin: BuiltinCall,
};

const BuiltinCall = struct {
    name: []const u8,
    args: []Expr,
};

const IfStmt = struct {
    cond: Expr,
    then_block: []Stmt,
    else_block: ?[]Stmt,
};

const Stmt = union(enum) {
    task_call: []const u8,
    builtin_call: BuiltinCall,
    if_stmt: IfStmt,
};

const VarDecl = struct {
    name: []const u8,
    value: Expr,
};

const Task = struct {
    name: []const u8,
    body: []Stmt,
};

const Program = struct {
    vars: []VarDecl,
    tasks: std.StringHashMap(Task),
    task_order: [][]const u8,
};

const Parser = struct {
    allocator: std.mem.Allocator,
    tokenizer: Tokenizer,
    current: Token,
    next_token: Token,

    fn init(allocator: std.mem.Allocator, src: []const u8) !Parser {
        var t = Tokenizer.init(allocator, src);
        const first = try t.next();
        const second = try t.next();
        return .{ .allocator = allocator, .tokenizer = t, .current = first, .next_token = second };
    }

    fn advance(self: *Parser) !void {
        self.current = self.next_token;
        self.next_token = try self.tokenizer.next();
    }

    fn expect(self: *Parser, kind: TokenKind) !Token {
        if (self.current.kind != kind) return error.UnexpectedToken;
        const tok = self.current;
        try self.advance();
        return tok;
    }

    fn parseProgram(self: *Parser) anyerror!Program {
        var var_list = try std.ArrayList(VarDecl).initCapacity(self.allocator, 0);
        var tasks = std.StringHashMap(Task).init(self.allocator);
        var order = try std.ArrayList([]const u8).initCapacity(self.allocator, 0);

        while (self.current.kind != .eof) {
            switch (self.current.kind) {
                .kw_string => {
                    const vd = try self.parseVarDecl();
                    try var_list.append(self.allocator, vd);
                },
                .kw_task => {
                    const task = try self.parseTask();
                    if (tasks.contains(task.name)) return error.DuplicateTask;
                    try tasks.put(task.name, task);
                    try order.append(self.allocator, task.name);
                },
                else => return error.UnexpectedToken,
            }
        }

        return .{ .vars = try var_list.toOwnedSlice(self.allocator), .tasks = tasks, .task_order = try order.toOwnedSlice(self.allocator) };
    }

    fn parseVarDecl(self: *Parser) anyerror!VarDecl {
        _ = try self.expect(.kw_string);
        const name_tok = try self.expect(.ident);
        _ = try self.expect(.eq);
        const value = try self.parseExpr();
        _ = try self.expect(.semicolon);
        return .{ .name = name_tok.lexeme, .value = value };
    }

    fn parseTask(self: *Parser) anyerror!Task {
        _ = try self.expect(.kw_task);
        const name_tok = try self.expect(.ident);
        _ = try self.expect(.lparen);
        _ = try self.expect(.rparen);
        const body = try self.parseBlock();
        return .{ .name = name_tok.lexeme, .body = body };
    }

    fn parseBlock(self: *Parser) anyerror![]Stmt {
        _ = try self.expect(.lbrace);
        var list = try std.ArrayList(Stmt).initCapacity(self.allocator, 0);
        while (self.current.kind != .rbrace) {
            const st = try self.parseStmt();
            try list.append(self.allocator, st);
        }
        _ = try self.expect(.rbrace);
        return try list.toOwnedSlice(self.allocator);
    }

    fn parseStmt(self: *Parser) anyerror!Stmt {
        switch (self.current.kind) {
            .kw_if => return try self.parseIf(),
            .at => {
                const call = try self.parseBuiltinCall();
                _ = try self.expect(.semicolon);
                return .{ .builtin_call = call };
            },
            .ident => {
                const name = self.current.lexeme;
                try self.advance();
                _ = try self.expect(.lparen);
                _ = try self.expect(.rparen);
                _ = try self.expect(.semicolon);
                return .{ .task_call = name };
            },
            else => return error.UnexpectedToken,
        }
    }

    fn parseIf(self: *Parser) anyerror!Stmt {
        _ = try self.expect(.kw_if);
        _ = try self.expect(.lparen);
        const cond = try self.parseExpr();
        _ = try self.expect(.rparen);
        const then_block = try self.parseBlock();
        var else_block: ?[]Stmt = null;
        if (self.current.kind == .kw_else) {
            _ = try self.expect(.kw_else);
            else_block = try self.parseBlock();
        }
        return .{ .if_stmt = .{ .cond = cond, .then_block = then_block, .else_block = else_block } };
    }

    fn parseExpr(self: *Parser) anyerror!Expr {
        switch (self.current.kind) {
            .string => {
                const s = self.current.str_value.?;
                try self.advance();
                return .{ .str = s };
            },
            .ident => {
                const name = self.current.lexeme;
                try self.advance();
                return .{ .var_ref = name };
            },
            .at => {
                const call = try self.parseBuiltinCall();
                return .{ .builtin = call };
            },
            else => return error.UnexpectedToken,
        }
    }

    fn parseBuiltinCall(self: *Parser) anyerror!BuiltinCall {
        _ = try self.expect(.at);
        const name_tok = try self.expect(.ident);
        _ = try self.expect(.lparen);
        var args = try std.ArrayList(Expr).initCapacity(self.allocator, 0);
        if (self.current.kind != .rparen) {
            while (true) {
                const e = try self.parseExpr();
                try args.append(self.allocator, e);
                if (self.current.kind == .comma) {
                    _ = try self.expect(.comma);
                    continue;
                }
                break;
            }
        }
        _ = try self.expect(.rparen);
        return .{ .name = name_tok.lexeme, .args = try args.toOwnedSlice(self.allocator) };
    }
};

const Executor = struct {
    allocator: std.mem.Allocator,
    io: std.Io,
    program: *const Program,
    vars: std.StringHashMap([]const u8),
    call_stack: std.ArrayList([]const u8),

    fn init(allocator: std.mem.Allocator, io: std.Io, program: *const Program) anyerror!Executor {
        return .{
            .allocator = allocator,
            .io = io,
            .program = program,
            .vars = std.StringHashMap([]const u8).init(allocator),
            .call_stack = try std.ArrayList([]const u8).initCapacity(allocator, 0),
        };
    }

    fn loadVars(self: *Executor) anyerror!void {
        for (self.program.vars) |vd| {
            const val = try self.evalString(vd.value);
            try self.vars.put(vd.name, val);
        }
    }

    fn execTask(self: *Executor, name: []const u8) anyerror!void {
        if (self.isInStack(name)) return error.RecursiveTask;
        const t = self.program.tasks.get(name) orelse return error.UnknownTask;
        try self.call_stack.append(self.allocator, name);
        for (t.body) |st| {
            try self.execStmt(st);
        }
        _ = self.call_stack.pop();
    }

    fn isInStack(self: *Executor, name: []const u8) bool {
        for (self.call_stack.items) |n| {
            if (std.mem.eql(u8, n, name)) return true;
        }
        return false;
    }

    fn execStmt(self: *Executor, st: Stmt) anyerror!void {
        switch (st) {
            .task_call => |name| try self.execTask(name),
            .builtin_call => |call| {
                _ = try self.evalBuiltin(call, false);
            },
            .if_stmt => |ifs| {
                const cond = try self.evalBool(ifs.cond);
                if (cond) {
                    for (ifs.then_block) |s| try self.execStmt(s);
                } else if (ifs.else_block) |blk| {
                    for (blk) |s| try self.execStmt(s);
                }
            },
        }
    }

    fn evalBool(self: *Executor, e: Expr) anyerror!bool {
        switch (e) {
            .builtin => |call| {
                const r = try self.evalBuiltin(call, true);
                return switch (r) {
                    .bool => |b| b,
                    .void => return error.InvalidExpr,
                };
            },
            .str => |s| return s.len != 0,
            .var_ref => |name| {
                const v = self.vars.get(name) orelse return error.UnknownVariable;
                return v.len != 0;
            },
        }
    }

    const BuiltinResult = union(enum) { void, bool: bool };

    fn evalBuiltin(self: *Executor, call: BuiltinCall, want_bool: bool) anyerror!BuiltinResult {
        const name = call.name;
        if (std.mem.eql(u8, name, "echo")) {
            if (want_bool) return error.InvalidExpr;
            const msg = try self.formatArgs(call.args);
            var stdout_writer = std.Io.File.stdout().writer(self.io, &.{});
            try stdout_writer.interface.print("{s}\n", .{msg});
            try stdout_writer.interface.flush();
            return .void;
        } else if (std.mem.eql(u8, name, "run")) {
            if (want_bool) return error.InvalidExpr;
            const cmd = try self.formatArgs(call.args);
            try self.runCommand(cmd);
            return .void;
        } else if (std.mem.eql(u8, name, "error")) {
            if (want_bool) return error.InvalidExpr;
            const msg = try self.formatArgs(call.args);
            var stderr_writer = std.Io.File.stderr().writer(self.io, &.{});
            try stderr_writer.interface.print("{s}\n", .{msg});
            try stderr_writer.interface.flush();
            return error.UserError;
        } else if (std.mem.eql(u8, name, "file_exists")) {
            if (call.args.len != 1) return error.InvalidArgs;
            const path = try self.evalString(call.args[0]);
            const exists = self.fileExists(path);
            if (want_bool) return .{ .bool = exists };
            return .void;
        } else if (std.mem.eql(u8, name, "question")) {
            const msg = try self.formatArgs(call.args);
            var stdout_writer = std.Io.File.stdout().writer(self.io, &.{});
            try stdout_writer.interface.print("{s} ", .{msg});
            try stdout_writer.interface.flush();
            var buf: [64]u8 = undefined;
            var stdin_reader = std.Io.File.stdin().reader(self.io, &.{});
            const n = try stdin_reader.interface.readSliceShort(&buf);
            const ans = std.mem.trim(u8, buf[0..n], " \t\r\n");
            const yes = ans.len > 0 and (ans[0] == 'y' or ans[0] == 'Y');
            if (want_bool) return .{ .bool = yes };
            return .void;
        } else if (std.mem.eql(u8, name, "is_windows")) {
            const yes = builtin.os.tag == .windows;
            if (want_bool) return .{ .bool = yes };
            return .void;
        } else if (std.mem.eql(u8, name, "is_outdated")) {
            if (call.args.len < 1) return error.InvalidArgs;
            const out_path = try self.evalString(call.args[0]);
            const outdated = try self.isOutdated(out_path, call.args[1..]);
            if (want_bool) return .{ .bool = outdated };
            return .void;
        } else if (std.mem.eql(u8, name, "is_outdated_depfile")) {
            if (call.args.len != 2) return error.InvalidArgs;
            const out_path = try self.evalString(call.args[0]);
            const dep_path = try self.evalString(call.args[1]);
            const outdated = try self.isOutdatedDepfile(out_path, dep_path);
            if (want_bool) return .{ .bool = outdated };
            return .void;
        }

        return error.UnknownBuiltin;
    }

    fn evalString(self: *Executor, e: Expr) anyerror![]const u8 {
        switch (e) {
            .str => |s| return s,
            .var_ref => |name| return self.vars.get(name) orelse return error.UnknownVariable,
            .builtin => return error.InvalidExpr,
        }
    }

    fn formatArgs(self: *Executor, args: []Expr) anyerror![]const u8 {
        if (args.len == 0) return error.InvalidArgs;
        const fmt = try self.evalString(args[0]);
        var vals = try std.ArrayList([]const u8).initCapacity(self.allocator, 0);
        for (args[1..]) |e| {
            const v = try self.evalString(e);
            try vals.append(self.allocator, v);
        }
        return try format(self.allocator, fmt, vals.items);
    }

    fn fileExists(self: *Executor, path: []const u8) bool {
        _ = std.Io.Dir.cwd().statFile(self.io, path, .{}) catch return false;
        return true;
    }

    fn isOutdated(self: *Executor, out_path: []const u8, deps: []Expr) anyerror!bool {
        const out_stat = std.Io.Dir.cwd().statFile(self.io, out_path, .{}) catch return true;
        const out_mtime = out_stat.mtime;
        if (deps.len == 0) return false;
        for (deps) |e| {
            const dep_path = try self.evalString(e);
            const dep_stat = std.Io.Dir.cwd().statFile(self.io, dep_path, .{}) catch return true;
            if (dep_stat.mtime.toNanoseconds() > out_mtime.toNanoseconds()) return true;
        }
        return false;
    }

    fn isOutdatedDepfile(self: *Executor, out_path: []const u8, depfile_path: []const u8) anyerror!bool {
        const out_stat = std.Io.Dir.cwd().statFile(self.io, out_path, .{}) catch return true;
        const out_mtime = out_stat.mtime;
        const depfile = std.Io.Dir.cwd().readFileAlloc(self.io, depfile_path, self.allocator, .limited(10 * 1024 * 1024)) catch return true;

        var seen_colon = false;
        var i: usize = 0;
        var tok = try std.ArrayList(u8).initCapacity(self.allocator, 0);

        while (i < depfile.len) {
            const c = depfile[i];
            if (!seen_colon) {
                if (c == ':') {
                    seen_colon = true;
                }
                i += 1;
                continue;
            }

            if (c == '\\') {
                if (i + 1 < depfile.len and depfile[i + 1] == '\n') {
                    i += 2;
                    continue;
                }
                if (i + 1 < depfile.len) {
                    i += 1;
                    try tok.append(self.allocator, depfile[i]);
                    i += 1;
                    continue;
                }
            }

            if (c == ' ' or c == '\t' or c == '\r' or c == '\n') {
                if (tok.items.len > 0) {
                    const dep_path = tok.items;
                    const dep_stat = std.Io.Dir.cwd().statFile(self.io, dep_path, .{}) catch return true;
                    if (dep_stat.mtime.toNanoseconds() > out_mtime.toNanoseconds()) return true;
                    tok.clearRetainingCapacity();
                }
                i += 1;
                continue;
            }

            try tok.append(self.allocator, c);
            i += 1;
        }

        if (tok.items.len > 0) {
            const dep_path = tok.items;
            const dep_stat = std.Io.Dir.cwd().statFile(self.io, dep_path, .{}) catch return true;
            if (dep_stat.mtime.toNanoseconds() > out_mtime.toNanoseconds()) return true;
        }

        if (!seen_colon) return true;
        return false;
    }

    fn runCommand(self: *Executor, cmd: []const u8) anyerror!void {
        if (builtin.os.tag == .windows) {
            var child = try std.process.spawn(self.io, .{
                .argv = &[_][]const u8{ "cmd.exe", "/C", cmd },
            });
            const term = try child.wait(self.io);
            switch (term) {
                .exited => |code| if (code != 0) return error.CommandFailed,
                else => return error.CommandFailed,
            }
            return;
        }
        var child = try std.process.spawn(self.io, .{
            .argv = &[_][]const u8{ "/bin/sh", "-c", cmd },
        });
        const term = try child.wait(self.io);
        switch (term) {
            .exited => |code| if (code != 0) return error.CommandFailed,
            else => return error.CommandFailed,
        }
    }
};

fn format(allocator: std.mem.Allocator, fmt: []const u8, args: [][]const u8) ![]const u8 {
    var out = try std.ArrayList(u8).initCapacity(allocator, 0);
    var i: usize = 0;
    var ai: usize = 0;
    while (i < fmt.len) {
        if (fmt[i] == '%' and i + 1 < fmt.len) {
            if (fmt[i + 1] == '%') {
                try out.append(allocator, '%');
                i += 2;
                continue;
            }

            var j = i + 1;
            var pad_char: u8 = ' ';
            var left_align = false;
            if (j < fmt.len and fmt[j] == '-') {
                left_align = true;
                j += 1;
            }
            if (j < fmt.len and fmt[j] == '0') {
                pad_char = '0';
                j += 1;
            }
            var width: usize = 0;
            while (j < fmt.len and fmt[j] >= '0' and fmt[j] <= '9') {
                width = width * 10 + (fmt[j] - '0');
                j += 1;
            }

            if (j < fmt.len) {
                const n = fmt[j];
                if (n == 's' or n == 'd' or n == 'i' or n == 'u' or n == 'x' or n == 'X' or n == 'b' or n == 'c') {
                    if (ai >= args.len) return error.FormatArgs;
                    const arg = args[ai];
                    ai += 1;
                    i = j + 1;

                    var buf: [256]u8 = undefined;
                    var slice: []const u8 = "";

                    if (n == 's') {
                        slice = arg;
                    } else if (n == 'c') {
                        const val = std.fmt.parseInt(u8, arg, 0) catch '?';
                        buf[0] = val;
                        slice = buf[0..1];
                    } else {
                        const val = std.fmt.parseInt(i64, arg, 0) catch 0;
                        slice = switch (n) {
                            'd', 'i' => std.fmt.bufPrint(&buf, "{d}", .{val}) catch "",
                            'u' => std.fmt.bufPrint(&buf, "{d}", .{@as(u64, @bitCast(val))}) catch "",
                            'x' => std.fmt.bufPrint(&buf, "{x}", .{val}) catch "",
                            'X' => std.fmt.bufPrint(&buf, "{X}", .{val}) catch "",
                            'b' => std.fmt.bufPrint(&buf, "{b}", .{val}) catch "",
                            else => unreachable,
                        };
                    }

                    if (width > slice.len) {
                        const pad_len = width - slice.len;
                        if (left_align) {
                            try out.appendSlice(allocator, slice);
                            try out.appendNTimes(allocator, ' ', pad_len);
                        } else {
                            try out.appendNTimes(allocator, pad_char, pad_len);
                            try out.appendSlice(allocator, slice);
                        }
                    } else {
                        try out.appendSlice(allocator, slice);
                    }
                    continue;
                }
            }
        }
        try out.append(allocator, fmt[i]);
        i += 1;
    }
    if (ai != args.len) return error.FormatArgs;
    return try out.toOwnedSlice(allocator);
}

fn usage(stdout: anytype) !void {
    try stdout.print(
        "lmake usage:\n  lmake [-f file] [target]\n  lmake --list\n",
        .{},
    );
    try stdout.flush();
}

fn runLmake(io: std.Io, stdout: anytype, arena_alloc: std.mem.Allocator, args: []const []const u8) !void {
    var file_path: []const u8 = "lmakefile";
    var target: ?[]const u8 = null;
    var list_only = false;

    var i: usize = 1;
    while (i < args.len) {
        const a = args[i];
        if (std.mem.eql(u8, a, "-h") or std.mem.eql(u8, a, "--help")) {
            try usage(stdout);
            return;
        }
        if (std.mem.eql(u8, a, "--list")) {
            list_only = true;
            i += 1;
            continue;
        }
        if (std.mem.eql(u8, a, "-f")) {
            if (i + 1 >= args.len) return error.InvalidArgs;
            file_path = args[i + 1];
            i += 2;
            continue;
        }
        if (target == null) {
            target = a;
            i += 1;
            continue;
        }
        return error.InvalidArgs;
    }

    const src = try std.Io.Dir.cwd().readFileAlloc(io, file_path, arena_alloc, .limited(10 * 1024 * 1024));

    var parser = try Parser.init(arena_alloc, src);
    var program = try parser.parseProgram();

    if (list_only) {
        for (program.task_order) |name| {
            try stdout.print("{s}\n", .{name});
        }
        try stdout.flush();
        return;
    }

    const entry: []const u8 = blk: {
        if (target) |t| break :blk t;
        if (program.tasks.contains("all")) break :blk "all";
        if (program.task_order.len > 0) break :blk program.task_order[0];
        return error.NoTasks;
    };

    var exec = try Executor.init(arena_alloc, io, &program);
    try exec.loadVars();
    try exec.execTask(entry);
}

pub export fn lk(argc: c_int, argv: [*][*:0]u8) c_int {
    const environ_slice: [:null]const ?[*:0]const u8 = std.mem.span(std.c.environ);
    const process_environ: std.process.Environ = .{
        .block = .{ .slice = environ_slice },
    };
    var gpa: std.heap.DebugAllocator(.{}) = .init;
    defer _ = gpa.deinit();
    var threaded: std.Io.Threaded = .init(gpa.allocator(), .{
        .environ = process_environ,
    });
    defer threaded.deinit();
    const io = threaded.io();
    var stdout_buffer: [256]u8 = undefined;
    var stdout_writer = std.Io.File.stdout().writer(io, &stdout_buffer);
    const stdout = &stdout_writer.interface;
    var stderr_writer = std.Io.File.stderr().writer(io, &.{});
    var arena = std.heap.ArenaAllocator.init(gpa.allocator());
    defer arena.deinit();
    const arena_alloc = arena.allocator();
    const arg_count: usize = if (argc > 0) @intCast(argc) else 0;
    const args = arena_alloc.alloc([]const u8, arg_count) catch {
        _ = stderr_writer.interface.print("lmake: OutOfMemory\n", .{}) catch {};
        _ = stderr_writer.interface.flush() catch {};
        return 1;
    };

    for (0..arg_count) |index| {
        args[index] = std.mem.span(argv[index]);
    }

    runLmake(io, stdout, arena_alloc, args) catch |err| {
        _ = stderr_writer.interface.print("lmake: {s}\n", .{@errorName(err)}) catch {};
        _ = stderr_writer.interface.flush() catch {};
        return 1;
    };
    return 0;
}
