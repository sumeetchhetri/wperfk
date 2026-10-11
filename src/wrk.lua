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

-- vegeta HTTP target format:
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
   if #targets == 0 then wrk.fail(file .. ": no targets") end

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
