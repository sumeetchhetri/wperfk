local wrk = {
   scheme  = "http",
   host    = "localhost",
   port    = nil,
   method  = "GET",
   path    = "/",
   headers = {},
   body    = nil,
   thread  = nil,
}

function wrk.resolve(host, service)
   local addrs = wrk.lookup(host, service)
   for i = #addrs, 1, -1 do
      if not wrk.connect(addrs[i]) then
         table.remove(addrs, i)
      end
   end
   wrk.addrs = addrs
end

function wrk.setup(thread)
   thread.addr = wrk.addrs[1]
   if type(setup) == "function" then
      setup(thread)
   end
end

function wrk.init(args)
   if not wrk.headers["Host"] then
      local host = wrk.host
      local port = wrk.port

      host = host:find(":") and ("[" .. host .. "]")  or host
      host = port           and (host .. ":" .. port) or host

      wrk.headers["Host"] = host
   end

   if type(init) == "function" then
      init(args)
   end

   if wrk.targets_file then
      if type(request) == "function" then
         wrk.fail("--targets cannot be combined with a script that defines request()")
      end
      local reqs = wrk.load_targets(wrk.targets_file)
      local n, i, p = #reqs, 0, (wrk.pipeline or 1)
      _G.request = function()
         if p == 1 then
            i = i % n + 1
            return reqs[i]
         end
         local batch = {}
         for k = 1, p do
            i = i % n + 1
            batch[k] = reqs[i]
         end
         return table.concat(batch)
      end
      return
   end

   local req = wrk.format()
   if (wrk.pipeline or 1) > 1 then
      req = string.rep(req, wrk.pipeline)
   end
   wrk.request = function()
      return req
   end
end

function wrk.format(method, path, headers, body)
   local method  = method  or wrk.method
   local path    = path    or wrk.path
   local headers = headers or wrk.headers
   local body    = body    or wrk.body
   local s       = {}

   if not headers["Host"] then
      headers["Host"] = wrk.headers["Host"]
   end

   headers["Content-Length"] = body and string.len(body)

   s[1] = string.format("%s %s HTTP/1.1", method, path)
   for name, value in pairs(headers) do
      s[#s+1] = string.format("%s: %s", name, value)
   end

   s[#s+1] = ""
   s[#s+1] = body or ""

   return table.concat(s, "\r\n")
end

function wrk.fail(msg)
   io.stderr:write(msg, "\n")
   os.exit(1)
end

local function origin(scheme, host, port)
   host = host:gsub("^%[(.*)%]$", "%1"):lower()
   return scheme .. "://" .. host .. ":" .. (port or (scheme == "https" and "443" or "80"))
end

local function format_target(method, path, headers, body)
   local s = { method .. " " .. path .. " HTTP/1.1" }
   if body then
      headers["Content-Length"] = #body
   end
   for name, value in pairs(headers) do
      s[#s+1] = name .. ": " .. value
   end
   s[#s+1] = ""
   s[#s+1] = body or ""
   return table.concat(s, "\r\n")
end

-- Minimal JSON decoder (for HAR files).
local function json_decode(s)
   local pos = 1
   local function fail(what)
      error(string.format("invalid JSON at byte %d: %s", pos, what), 0)
   end
   local function ws()
      pos = s:find("[^ \t\r\n]", pos) or #s + 1
   end
   local escapes = { ['"'] = '"', ["\\"] = "\\", ["/"] = "/", b = "\b", f = "\f", n = "\n", r = "\r", t = "\t" }
   local function utf8(cp)
      if cp < 0x80 then return string.char(cp) end
      if cp < 0x800 then return string.char(0xC0 + math.floor(cp / 0x40), 0x80 + cp % 0x40) end
      if cp < 0x10000 then
         return string.char(0xE0 + math.floor(cp / 0x1000), 0x80 + math.floor(cp / 0x40) % 0x40, 0x80 + cp % 0x40)
      end
      return string.char(0xF0 + math.floor(cp / 0x40000), 0x80 + math.floor(cp / 0x1000) % 0x40,
                         0x80 + math.floor(cp / 0x40) % 0x40, 0x80 + cp % 0x40)
   end
   local value
   local function str()
      local out, i = {}, pos + 1
      while true do
         local j = s:find('["\\]', i)
         if not j then fail("unterminated string") end
         out[#out+1] = s:sub(i, j - 1)
         if s:sub(j, j) == '"' then pos = j + 1; return table.concat(out) end
         local e = s:sub(j + 1, j + 1)
         if e == "u" then
            local cp = tonumber(s:sub(j + 2, j + 5), 16) or fail("bad \\u escape")
            i = j + 6
            if cp >= 0xD800 and cp < 0xDC00 and s:sub(i, i + 1) == "\\u" then
               local lo = tonumber(s:sub(i + 2, i + 5), 16)
               if lo and lo >= 0xDC00 and lo < 0xE000 then
                  cp = 0x10000 + (cp - 0xD800) * 0x400 + (lo - 0xDC00)
                  i = i + 6
               end
            end
            out[#out+1] = utf8(cp)
         else
            out[#out+1] = escapes[e] or fail("bad escape")
            i = j + 2
         end
      end
   end
   function value()
      ws()
      local c = s:sub(pos, pos)
      if c == "{" then
         local t = {}
         pos = pos + 1; ws()
         if s:sub(pos, pos) == "}" then pos = pos + 1; return t end
         while true do
            ws()
            if s:sub(pos, pos) ~= '"' then fail("expected key") end
            local k = str()
            ws()
            if s:sub(pos, pos) ~= ":" then fail("expected ':'") end
            pos = pos + 1
            t[k] = value()
            ws()
            c = s:sub(pos, pos); pos = pos + 1
            if c == "}" then return t end
            if c ~= "," then fail("expected ',' or '}'") end
         end
      elseif c == "[" then
         local t = {}
         pos = pos + 1; ws()
         if s:sub(pos, pos) == "]" then pos = pos + 1; return t end
         while true do
            t[#t+1] = value()
            ws()
            c = s:sub(pos, pos); pos = pos + 1
            if c == "]" then return t end
            if c ~= "," then fail("expected ',' or ']'") end
         end
      elseif c == '"' then
         return str()
      elseif s:sub(pos, pos + 3) == "true" then pos = pos + 4; return true
      elseif s:sub(pos, pos + 4) == "false" then pos = pos + 5; return false
      elseif s:sub(pos, pos + 3) == "null" then pos = pos + 4; return nil
      else
         local num = s:match("^-?%d+%.?%d*[eE]?[-+]?%d*", pos)
         if not num or num == "" then fail("unexpected '" .. c .. "'") end
         pos = pos + #num
         return tonumber(num)
      end
   end
   local result = value()
   ws()
   if pos <= #s then fail("trailing data") end
   return result
end

-- HAR (HTTP Archive, e.g. browser devtools "Save all as HAR"): the requests
-- on the target origin become targets; other origins are skipped.
local skip_har_header = { host = true, ["content-length"] = true, connection = true,
                          ["transfer-encoding"] = true, ["keep-alive"] = true }

local function load_har(file, data, want)
   local ok, har = pcall(json_decode, data)
   if not ok then wrk.fail(file .. ": " .. har) end
   local entries = har and har.log and har.log.entries
   if type(entries) ~= "table" then wrk.fail(file .. ": not a HAR file (no log.entries)") end

   local targets, skipped = {}, 0
   for _, e in ipairs(entries) do
      local r = e.request or {}
      local scheme, auth, path = tostring(r.url or ""):match("^(https?)://([^/?#]+)([^#]*)")
      local host, port
      if scheme then
         host, port = auth:match("^(%[.+%]):(%d+)$")
         if not host then host, port = auth:match("^([^:]+):(%d+)$") end
         host = (host or auth):gsub("^.*@", "")
      end
      if scheme and r.method and origin(scheme, host, port) == want then
         -- percent-encode anything outside printable ASCII (hand-edited HARs)
         path = path:gsub("[^\33-\126]", function(c) return string.format("%%%02X", c:byte()) end)
         local t = { method = r.method, path = path ~= "" and path or "/", headers = {} }
         for _, h in ipairs(r.headers or {}) do
            local name = tostring(h.name or "")
            if name ~= "" and name:sub(1, 1) ~= ":" and not skip_har_header[name:lower()] then
               t.headers[name] = tostring(h.value or "")
            end
         end
         if r.postData and r.postData.text then t.body = r.postData.text end
         targets[#targets+1] = t
      else
         skipped = skipped + 1
      end
   end
   if skipped > 0 then
      io.stderr:write(string.format("%s: skipped %d request(s) not on %s\n", file, skipped, want))
   end
   return targets
end

-- vegeta HTTP target format:
-- (or a HAR file, detected by its leading '{')
--   METHOD URL
--   Header: value      (any number)
--   @path/to/body      (optional)
--   <blank line>
function wrk.load_targets(file)
   local f, err = io.open(file, "rb")
   if not f then wrk.fail("unable to read targets: " .. tostring(err)) end
   local data = f:read("*a")
   f:close()

   local want = origin(wrk.scheme, wrk.host, wrk.port)
   local targets, cur, n = {}, nil, 0
   if data:match("^%s*{") then
      targets = load_har(file, data, want)
      data = ""
   end
   for line in (data .. "\n"):gmatch("([^\n]*)\n") do
      n = n + 1
      line = line:gsub("\r$", "")
      local where = file .. ":" .. n .. ": "
      if line:match("^%s*#") then
         -- comment
      elseif line:match("^%s*$") then
         cur = nil
      elseif not cur then
         local method, url = line:match("^%s*(%u+)%s+(%S+)%s*$")
         if not method then wrk.fail(where .. "expected 'METHOD URL'") end
         local scheme, auth, path = url:match("^(https?)://([^/?#]+)(.*)$")
         if not scheme then wrk.fail(where .. "expected an http:// or https:// URL") end
         local host, port = auth:match("^(%[.+%]):(%d+)$")
         if not host then host, port = auth:match("^([^:]+):(%d+)$") end
         host = host or auth
         if origin(scheme, host, port) ~= want then
            wrk.fail(where .. url .. " is not on " .. want .. " (all targets must share one origin)")
         end
         if path == "" then path = "/" elseif path:sub(1, 1) ~= "/" then path = "/" .. path end
         cur = { method = method, path = path, headers = {} }
         targets[#targets+1] = cur
      elseif line:sub(1, 1) == "@" then
         local bf, berr = io.open(line:sub(2), "rb")
         if not bf then wrk.fail(where .. "unable to read body: " .. tostring(berr)) end
         cur.body = bf:read("*a")
         bf:close()
      else
         local name, value = line:match("^%s*([^:]+):%s*(.-)%s*$")
         if not name then wrk.fail(where .. "expected 'Header: value' or '@bodyfile'") end
         cur.headers[name] = value
      end
   end
   if #targets == 0 then wrk.fail(file .. ": no targets on " .. want) end

   local reqs = {}
   for i, t in ipairs(targets) do
      local headers = {}
      for name, value in pairs(wrk.headers) do headers[name] = value end
      for name, value in pairs(t.headers) do headers[name] = value end
      reqs[i] = format_target(t.method, t.path, headers, t.body or wrk.body)
   end
   return reqs
end

return wrk
