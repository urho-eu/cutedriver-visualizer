############################################################################
##
## Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
## All rights reserved.
## Contact: Nokia Corporation (testabilitydriver@nokia.com)
##
## This file is part of Testability Driver.
##
## If you have questions regarding the use of this file, please contact
## Nokia at testabilitydriver@nokia.com .
##
## This library is free software; you can redistribute it and/or
## modify it under the terms of the GNU Lesser General Public
## License version 2.1 as published by the Free Software Foundation
## and appearing in the file LICENSE.LGPL included in the packaging
## of this file.
##
############################################################################

############################################################################
# Initializations and requires
############################################################################

# convenience debug methods (needed until Ruby 1.9)
module Kernel
private
  def this_method
    caller[0] =~ /`([^']*)'/ and $1
  end
  def calling_method
    caller[1] =~ /`([^']*)'/ and $1
  end
end

require 'logger'

if /win/ =~ RUBY_PLATFORM
  $lg = Logger.new('C:/temp/visualizer_tdriver_interface.log', 'daily')
else
  $lg = Logger.new('/tmp/visualizer_tdriver_interface.log', 'daily')
end

$lg.level = Logger::DEBUG
$lg.debug " ==================================== OPEN"


require 'benchmark'
require 'socket'


begin
  require 'tdriver'
  @tdriver_require_error = "" # no error
rescue LoadError => ex
  # do not change the following string
  @tdriver_require_error = "LoadError: tdriver GEM\n#{ ex.message }"
rescue => ex
  @tdriver_require_error = "#{ ex.class }: #{ ex.message }\n\nBacktrace: #{ ex.backtrace.join("_") }"
end


if @tdriver_require_error.empty?
  @tdriver_gem_version = ENV['TDRIVER_VERSION']
else
  @tdriver_gem_version = 'error'
end

@tdriver_interface_rb_version = '1'
@server = TCPServer.new("127.0.0.1", 0)

# stdout printout format defined by list below:
# entries must be in that order, separated by whitespace
# first line is to be split by whitespaces, after which string list indexes are:
# 0: "TDriverVisualizerRubyInterface"
# 1: "version"
# 2: version number of script interface, to be changed with incompatible changes
# 3: "port"
# 4: port on localhost where script listens for client connection
# 5: "tdriver"
# 6: version string if tdriver required ok, "error" otherwise, with error dump starting from second line
hellolist = [
  'TDriverVisualizerRubyInterface',
  'version', @tdriver_interface_rb_version.to_s, # protocol version, increase for incompatible changes
  'port', @server.addr[1].to_s, # port on localhost where script listens for client connection
  'tdriver', @tdriver_gem_version.to_s ] # tdriver version string, or "error" if require tdriver failed
hellostring = hellolist.join(' ')
STDOUT.puts hellostring

if @tdriver_gem_version == 'error' or not @tdriver_require_error.empty?
  $lg.fatal hellostring
  $lg.fatal @tdriver_require_error
  STDOUT.puts
  exit 1
end

$lg.info hellostring
STDOUT.flush


############################################################################
# Ruby implementation of protocol used by C++ class TDriverRbcProtocol
############################################################################


# message format:
#   Notation:
#     N: unsigned 4-byte integer in network byte order
#     N: .. { .. } a repating block, total length given by N
#     NA*: a text string, length in bytes given by N
#
#   Format:
#     N: sequence number
#     NA*: name string, also implies format of raw data, though there's only one format currently, QMap<QString, QStringList>
#     N: raw data {
#       NA*: key string
#       N: raw list data {
#         NA*: list item string
#       }
#     }


def readWord(socket)
  # works like Qt QDataStream::operator>> for quint32
  buf = ""
  begin
    while buf.length != 4
      data, =  socket.recvfrom(4 - buf.length)
      raise "Connection closed" if data.length == 0
      buf += data
    end
  rescue Errno::EAGAIN, Errno::EINTR
    #STDERR.puts "recvfrom reading len, recoverable exception"
    $lg.debug this_method + " recoverable exception"
    IO.select([socket])
    retry
  end

  word = buf.unpack('N')[0] # N=4 byte unsigned in network byte order
  return word
end


def readBytes(socket)
  # works like Qt QDataStream::readBytes, uses network byte order
  # returns raw data read

  len = readWord(socket)

  return '' if len==0 or len == 0xFFFFFFFF

  buf = ""
  begin
    while buf.length < len
      data, = socket.recvfrom(len - buf.length)
      raise "Connection closed" if data.length == 0
      buf += data
    end
  rescue Errno::EAGAIN, Errno::EINTR
    #STDERR.puts "recvfrom reading data, recoverable exception"
    $lg.debug this_method + " recoverable exception"
    IO.select([socket])
    retry
  end

  return buf
end


def readMessage(socket)
  seqNum = readWord(socket)
  name = readBytes(socket)
  data = readBytes(socket)
  return seqNum, name, data
end


def writeRawData(socket, buf)
  len = buf.length
  while len > 0
    written = socket.write(buf)
    if written > 0 then
      len -= written
    else
      STDERR.puts "bad write, returned #{written}, fatal!"
      socket.close # causes the script to exit
    end
  end
end


def makeMsg(seqnum, name, map)
  mapdata = ""
  map.each do |key, value|
    key_s = key.to_s
    itemdata = ""
    value.to_a.each do |item|
      item_s = item.to_s
      itemdata += [item_s.length, item_s].pack('NA*')
    end
    mapdata += [ key_s.length, key_s].pack('NA*')+[itemdata.length, itemdata].pack('NA*')
  end
  data = [seqnum, name.length, name].pack('NNA*')+[mapdata.length, mapdata].pack('NA*')
  #STDERR.puts "Creating message: #{seqnum} #{name.length} #{mapdata.length} -> #{4+(4+name.length)+(4+mapdata.length)} ?= #{data.length}"
  return data
end


def parseBytes(data, offset)
  len = data[offset, 4].unpack('N')[0] # N=4 byte unsigned in network byte order
  len = 0 if len == 0xFFFFFFFF

  offset += 4
  if len > 0
    bytes = data[offset, len].unpack('A*')[0]
  else
    bytes = ''
  end
  return bytes, offset+len
end


def parseFullMsg(msgdata)
  seqnum = data[0, 4].unpack('N')[0] # N=4 byte unsigned in network byte order
  name, offset = parseBytes(msgdata, offset=4)
  mapdata, offset = parseBytes(msgdata, offset)
  return seqnum, name, mapdata
end


def parseArray(data)
    offset = 0
    list = Array.new

    while offset < data.length
      item, offset = parseBytes(data, offset)
      list << item
    end

    return list
end


def parseArrayHash(data)
  offset = 0
  map = Hash.new

  while offset < data.length
    key, offset = parseBytes(data, offset)

    listdata, offset = parseBytes(data, offset)
    map[key] = parseArray(listdata)
  end

  return map
end


############################################################################
# old tdriver_rubyinteract.rb
############################################################################


# class TDriver
#   class << self
#     alias __original_connect_sut__ connect_sut
#   end
#   def self.connect_sut( sut_attributes = {} )
#     STDERR.puts "Setting timeout to 0 for #{sut_attributes[ :Id ].to_sym}"
#     sut=self.__original_connect_sut__(sut_attributes)
#     sut.instance_eval{ @_testObjectFactory.timeout = 0 }
#     return sut
#   end
# end


class Code_evaluation_sandbox

def initialize
  @ruby_interact_binding = binding
  @ruby_keywords = '
BEGIN
END
__ENCODING__
__END__
__FILE__
__LINE__
alias
and
begin
break
case
class
def
defined?
do
else
elsif
end
ensure
false
for
if
in
module
next
nil
not
or
redo
rescue
retry
return
self
super
then
true
undef
unless
until
when
while
yield
'.split #'

  @ruby_classes = '
MatchingData
SizedQueue
EOFError
Proc
RegexpError
Module
ZeroDivisionError
SignalException
Array
LoadError
Method
Continuation
Exception
Queue
StringIO
Range
IndexError
SystemCallError
UnboundMethod
Mutex
SystemExit
NoMethodError
FloatDomainError
Class
IO
ThreadError
NilClass
Data
Interrupt
Win32API
NotImplementedError
Symbol
RangeError
Dir
SLex
Rational
StopIteration
Integer
Fixnum
ConditionVariable
Thread
Numeric
TrueClass
StandardError
File
RuntimeError
NameError
Struct
RubyLex
Object
Float
Time
ScriptError
Bignum
LocalJumpError
TypeError
ThreadGroup
SecurityError
MatchData
SystemStackError
IOError
Binding
Date
String
SyntaxError
Hash
FalseClass
ArgumentError
DateTime
NoMemoryError
Regexp
'.split #'



  @split_re = /^(.*?)([A-Za-z0-9_?!]*)$/
# the '?' in (.*?) makes '*' non-greedy above

  @implicit_filter_re = /^([^a-zA-Z_]|__wrap)/

end # Code_evaluation_sandbox#initialize


def line_completion(code, seqNum)
  code = code.to_s.rstrip
  if code.empty? then
    return { 'result_keys' => [ 'instance_variables', 'methods', 'ruby_keywords', 'ruby_classes' ],
             'instance_variables' => instance_eval('instance_variables'),
             'methods' => (methods | Kernel.methods),
             'ruby_keywords' => @ruby_keywords }
  end

    $lg.debug this_method + " code '#{code}'"

  m = @split_re.match(code)
  if m.size != 3 or not ( m[1].end_with?('.') or m[1].empty?) then
    return { 'result_keys' => [],
             'error_rubycode' => [ code ],
             'error_message' => [ 'Failed at code analyzation.'],
             'help_message' => ['The code to be completed must have a dot. Completion is done after the last dot.'] }
  end

  STDOUT.puts("\032\032START #{seqNum}\032")
  STDERR.puts("\032\032START #{seqNum}\032")
  ex = nil
  begin
    list = instance_eval { | | @ruby_interact_binding.eval("#{m[1]}methods") }
  rescue => ex
    STDERR.puts("Exception: #{ex}")
  end
  STDOUT.puts("\032\032END #{seqNum}\032")
  STDERR.puts("\032\032END #{seqNum}\032")
  STDOUT.flush
  STDERR.flush

  if ex != nil then
    return { 'result_keys' => [],
             'error_rubycode' => [ code ],
             'error_message' => [ 'Exception:', ex.to_s, ex.backtrace.join('\n') ],
             'help_message' => ['The code to be completed must not raise exceptions.'] }
  else
    method_list = Array.new
    list.find_all { |item|
      item.start_with?(m[2]) and !@implicit_filter_re.match(item)
    }.sort.each { |item|
      method_list << item.slice(m[2].size, item.size)
    }
    return { 'result_keys' => [ 'methods' ],
             'methods' => method_list }
  end
end


def line_execution(code, seqNum)
  code = code.to_s.rstrip
  #STDERR.puts "\032\032EVAL: " + code.dump + "\032"
  STDOUT.puts("\032\032START #{seqNum}\032")
  STDERR.puts("\032\032START #{seqNum}\032")
  ex = nil
  begin
    STDERR.puts "Evaluating: #{code.dump}"
    result = instance_eval { | | @ruby_interact_binding.eval(code) }
  rescue => ex
    STDERR.puts("Exception: #{ex}")
  end
  STDERR.puts("\032\032END #{seqNum}\032")
  STDERR.flush
  STDOUT.puts("\032\032END #{seqNum}\032")
  STDOUT.flush

  if ex != nil then
    return { 'error_rubycode' => [ code ],
             'error_message' => [ 'Exception:', ex.to_s ],
             'help_message' => ['The evaluated code should not raise exceptions.'] }
  else
    return { 'code' => [ code ], 'result class' => [ result.class ] }
             #'result inspect' => [ result.inspect ]
             #'result printable' => [ result.inspect.dump ]
  end
end


def invalidcmd(input)
    return { 'error_message' => [ 'bad request:', input.to_s ] }
end

end # class Code_evaluation_sandbox


############################################################################
# old listener.rb
############################################################################

@listener = Object.new

def @listener.set_working_directory( dir )
  @working_directory = dir
end


# set directory where to save xml & png
if /win/ =~ Config::CONFIG[ 'target_os' ] && /darwin/io !~ Config::CONFIG[ 'target_os' ]
  # windows
  @listener.set_working_directory( File.expand_path( ENV[ 'TEMP' ] ) )
else
  # unix/linux/other
  @listener.set_working_directory( File.expand_path( '/tmp/' ) )
end
@listener.instance_eval { | | $lg.debug "initial working directory: " + @working_directory }

def @listener.check_version
  @listener_reply['version'] = [ ENV['TDRIVER_VERSION'] ]
end


def @listener.check_api_fixture( sut )
  return sut.application.fixture('tasqtapiaccessor', 'version' )
end


def @listener.get_behaviours_xml( sut, sut_id, object_types )
  _output_filename_xml = File.join( @working_directory, "visualizer_behaviours_#{ sut_id }.xml" )
  # remove old file first
  File.delete( _output_filename_xml ) if File.exist?( _output_filename_xml )
  behaviour_attributes_hash = { :input_type => ['*', sut.input.to_s ], :sut_type => [ '*', sut.ui_type.upcase ], :version => [ '*', sut.ui_version ] }
  behaviours_xml = ""
  object_types.each do | object_type |
    behaviours_xml <<
      "<behaviour object_type=\"#{ object_type.to_s }\">\n" <<
        MobyUtil::XML::parse_string(
          MobyBase::BehaviourFactory.instance.to_xml( behaviour_attributes_hash.merge( { :object_type => ( object_type == 'sut' ? [ 'sut' ] : [ '*', object_type ] ) } ) )
        ).root.xpath('/behaviours/behaviour/object_methods/object_method').to_s <<
      "\n</behaviour>\n"
  end

  File.open( _output_filename_xml, 'w') do | file |
    file << MobyUtil::XML::parse_string( "<behaviours>\n#{ behaviours_xml }\n</behaviours>" ).to_s
    file.close
    $lg.debug this_method + " wrote #{File.size?(_output_filename_xml)/1024.0} KiB to '#{_output_filename_xml}'"
  end
end


def @listener.get_fixture_xml( sut, sut_id, object_name )
  _output_filename_xml = File.join( @working_directory, "visualizer_class_methods_#{ sut_id }.xml" )
  File.delete( _output_filename_xml ) if File.exist?( _output_filename_xml )
  data = sut.application.fixture('tasqtapiaccessor', 'list_class_methods', { :class => object_name } )
  File.open( _output_filename_xml, 'w') do | file |
    file << data
    file.close
    $lg.debug this_method + " wrote #{File.size?(_output_filename_xml)/1024.0} KiB to '#{_output_filename_xml}'"
  end
end


def @listener.get_signal_xml( sut, sut_id, app_name, object_id, object_type )
  $lg.debug "#{this_method} ENTRY #{Time.now}"
  _output_filename_xml = File.join( @working_directory, "visualizer_class_signals_#{ sut_id }.xml" )
  File.delete( _output_filename_xml ) if File.exist?( _output_filename_xml )
  cmd = "sut.application(:name => app_name).#{object_type}( :id => object_id ).fixture('signal', 'list_signals')"
  $lg.debug this_method + " eval '#{cmd}'"
  data = eval(cmd)
  $lg.debug "#{this_method} DATA #{Time.now}"
  File.open( _output_filename_xml, 'w') do | file |
    file << data
    file.close
    $lg.debug this_method + " wrote #{File.size?(_output_filename_xml)/1024.0} KiB to '#{_output_filename_xml}'"
  end
  $lg.debug "#{this_method} EXIT #{Time.now}"
end


def @listener.get_ui_dump( sut, sut_id, app_id = nil )
  MobyUtil::Parameter[ sut.id ][ :filter_type] = 'none'
  _output_filename_xml = File.join( @working_directory, "visualizer_dump_#{ sut_id }" )
  _output_filename_png = _output_filename_xml + '.png'
  _output_filename_xml += '.xml'
  data = nil
  benchtime = Benchmark.measure {
    data = sut.get_ui_dump( *[ ( { :id => app_id } unless app_id.nil? ) ].compact )
  }.real
  $lg.debug this_method + " sut.get_ui_dump time: #{benchtime}"

  File.open( _output_filename_xml, 'w') do | file |
    file << data
    file.close
    $lg.debug this_method + " wrote #{File.size?(_output_filename_xml)/1024.0} KiB to '#{_output_filename_xml}'"
  end

  begin
    if app_id.nil?
      benchtime = Benchmark.measure {
        sut.capture_screen( :Filename => _output_filename_png, :Redraw => true )
      }.real
      $lg.debug this_method + " sut.capture_screen time: #{benchtime}"

    else
      begin
        benchtime = Benchmark.measure {
          sut.application( :id => app_id ).capture_screen( "PNG", _output_filename_png, true )
        }.real
        $lg.debug this_method + " app.capture_screen time: #{benchtime}"
      rescue
        app_id = nil
        sut.capture_screen( :Filename => _output_filename_png, :Redraw => true )
      end
      $lg.debug this_method + " got #{File.size?(_output_filename_png)/1024.0} KiB in '#{_output_filename_png}'"

    end

  rescue => ex
    # screen capture failed
    File.delete(_output_filename_png) if File.exist?(_output_filename_png )
    # raise exception
    raise ex unless ex.message == "QtTasserver does not support the given service: screenShot"

  end

end


def @listener.get_app_list( sut, sut_id )
  _output_filename_xml = File.join( @working_directory, "visualizer_applications_#{ sut_id }.xml" )
  File.delete( _output_filename_xml ) if File.exist?( _output_filename_xml )
  # retrieve list of running applications
  output = sut.list_apps
  # write list to file
  File.open( _output_filename_xml, 'w') do | file |
      file << output
      file.close
    end
end


def @listener.start_recording( sut, app_id )
  MobyUtil::Recorder.start_rec( sut.application( :id => app_id ) )
end


def @listener.get_recorded_script( sut, app_id )
  application = sut.application( :id => app_id )
  _output_filename_rb = File.join( @working_directory, 'visualizer_rec_fragment.rb' )
  script = MobyUtil::Recorder.print_script( sut, application )
  File.open( _output_filename_rb, 'w') do | file |
    file << script
    file.close
    $lg.debug this_method + " wrote #{File.size?(_output_filename_rb)/1024.0} KiB to '#{_output_filename_rb}'"
  end
end


def @listener.test_script( sut )
  _output_filename_rb = File.join( @working_directory, 'visualizer_rec_fragment.rb' )
  File.new( _output_filename_rb ).each_line{ | line | eval( line ) }
end


def @listener.get_parameter( sut_id, para )
  para_value = MobyUtil::Parameter[ sut_id.to_sym ][ para.to_sym, nil ]
  @listener_reply['parameter'] = [ para.to_s, para_value.to_s ]
end


def @listener.set_output_path( new_path )
  old = @working_directory
  set_working_directory( MobyUtil::FileHelper.fix_path( File.expand_path( new_path.to_s ) + "/" ) )
  $lg.debug this_method + " @working_directory changed '#{old}' -> '#{@working_directory}'"
  @listener_reply['output_path'] = [ @working_directory.to_s ]
end


def @listener.main_loop (conn)
  recorder = nil
  interact = Code_evaluation_sandbox.new

  while not conn.closed? do
    STDOUT.flush
    STDERR.flush
    $lg.debug this_method + " reading message"
    seqNumIn = nameIn = dataIn = nil
    benchtime = Benchmark.measure {
      seqNumIn, nameIn, dataIn = readMessage(conn)
    }.real
    $lg.debug this_method + " got message after time: #{benchtime}"
    msgIn = parseArrayHash(dataIn)
    $lg.debug this_method + " #{seqNumIn} #{nameIn} " + msgIn.inspect
    #STDERR.puts "RUBY RECEIVED #{seqNumIn} #{nameIn} #{parseArrayHash(dataIn)}"

    #listener.rb was old script, which had STDIN/STDOUT interface
    if (nameIn == 'listener.rb emulation' and msgIn.key?('input') and not (input_array = msgIn['input']).empty?)
    then
      @listener_reply = Hash.new
      # handle commands where input_array length is 1
      break if ( input_array[0] == "quit" )

      if input_array.size >= 2
        sleep 1
        sut_id = input_array.first.to_sym
        cmd = input_array[ 1 ].downcase.to_sym
        eval_cmd = ""
        error = false

        begin
          # connect to sut, unless command does not require it
          sut = TDriver.connect_sut( :Id => sut_id ) unless [ :get_parameter, :set_output_path, :check_version ].include?( cmd )

          begin
            MobyUtil::Parameter[ sut.id ][ :filter_type] = 'none' if sut
          rescue
          end

        rescue => ex
          @listener_reply['exception'] = [ ex.message.to_s, ex.class.to_s, ex.backtrace.join('\n') ]
          @listener_reply['error'] = [ "Error: connection to sut (#{sut_id}) failed" ]
          error = true
        end

        if not error

          case cmd

          when :check_version
            eval_cmd = "check_version"

          when :set_output_path
            eval_cmd = "set_output_path( '#{ input_array[ 2 ] }' )"

          when :get_behaviours
            eval_cmd = "get_behaviours_xml( sut, '#{ sut_id }', [#{  input_array[ 2 ] }] )"

          when :refresh
            eval_cmd = "get_ui_dump( sut, '#{ sut_id.to_s }', #{ input_array.size > 2 ? "'#{ input_array[ 2 ] }'" : "nil" } )"

          when :list_apps
            eval_cmd = "get_app_list( sut, '#{ sut_id }' )"

          when :disconnect
            eval_cmd = "TDriver.disconnect_sut( :Id => '#{ sut_id }' )" # this does not work with qt

          when :get_parameter
            eval_cmd = "get_parameter( sut_id , '#{ input_array[ 2 ] }' )"

          when :tap_screen
            eval_cmd = "sut.tap_screen( #{ input_array[ 2 ].to_i }, #{ input_array[ 3 ].to_i } )"

          when :tap
            eval_cmd = "sut.application#{ input_array.size > 3 ? "( :id => '#{ input_array[ 3 ] }' ).#{ input_array[ 2 ] }" : ".#{ input_array[ 2 ] }" }.tap"

          when :check_fixture
            eval_cmd = "check_api_fixture( sut )"

          when :fixture
            #eval_cmd = "sut.application#{ input_array[ 2 ].downcase == 'application' ? "#{ input_array[ 3 ] }" : ".#{ input_array[ 2 ] }#{ input_array[ 3 ] }" }.fixture( 'tasqtapiaccessor', 'list_class_methods', { :class => '#{ input_array[ 2 ] }' } )"
            eval_cmd = "get_fixture_xml( sut, '#{ sut_id }', '#{ input_array[ 2 ] }' )"

          when :take_screen_shot
            eval_cmd = "sut.application.#{ input_array[ 2 ] }.capture_screen( 'PNG', '#{ File.join( @working_directory, 'visualizer_dump_#{ sut_id }.png', true) }' )"

          when :press_key
            eval_cmd = "sut.press_key( #{ input_array[ 2 ].to_sym } )"

          when :list_signals
            eval_cmd = "get_signal_xml( sut, '#{ sut_id }', '#{ input_array[ 2 ] }', '#{ input_array[ 3 ] }', '#{ input_array[ 4 ] }')"

          when :set_attribute
            parameter = ( input_array.size > 5 ? input_array[ 5..input_array.size ].join(" ") : parameter = input_array[ 5 ] )
            # strip single quotes added by visualizer
            parameter = parameter [ 1..-2 ] if parameter.size > 1
            eval_cmd = "sut.application.#{ input_array[ 2 ] }.set_attribute( '#{ input_array[ 4 ] }', '#{ parameter }', '#{input_array [ 3 ] }' )"

          when :start_record
            eval_cmd = "start_recording(sut, #{ ( input_array.size > 2 ? "'#{ input_array[ 2 ] }'" : "nil" ) })"

          when :stop_record
            eval_cmd = "get_recorded_script(sut, #{ ( input_array.size > 2 ? "'#{ input_array[ 2 ]}'" : "nil" ) })"

          when :test_record
            eval_cmd = "test_script( sut )"

          else
            @listener_reply['exception'] = []
            @listener_reply['error'] = [ "Error: no command matched (#{cmd})" ]
            error = true

          end #case cmd

          if not error and not eval_cmd.empty?
            benchtime = 'timing error'
            begin
              MobyUtil::Retryable.while( :times => 10, :timeout => 1, :exception => Exception ) do
                $lg.debug this_method + " cmd #{cmd} => eval_cmd '#{eval_cmd}'"
                benchtime = Benchmark.measure {
                  eval( eval_cmd )
                }.real
              end
            rescue => ex
              @listener_reply['exception'] = [ ex.message.to_s, ex.class.to_s, ex.backtrace.join('\n') ]
              @listener_reply['error'] = [ "Error: evaluating command (#{eval_cmd}) failed" ]
              error = true
            end
            $lg.debug this_method + " eval time: #{benchtime}"
          end

        else
        # error== true, because TDriver.connect_sut failed

        end # if !error

      else
        @listener_reply['exception'] = []
        @listener_reply['error'] = [ "Error: not enough parameters in command (#{cmd})" ]
        error = true

      end # if array_size >= 2

      $lg.debug " @listener_reply: #{@listener_reply.inspect}"

      # response to "listener.rb emulation" message
      msgOut = @listener_reply

    #ruby_interact.rb was old script, which had STDIN/STDOUT interface
    elsif (nameIn == 'ruby_interact.rb emulation' and
              msgIn.key?('command') and
              not (inputcmd = msgIn['command']).empty?)
    then
      case inputcmd[0]
      when "line_completion" : msgOut = interact.line_completion(inputcmd[1], seqNumIn)
      when "line_execution" : msgOut = interact.line_execution(inputcmd[1], seqNumIn)
      else msgOut = interact.invalidcmd(inputcmd)
      end

    elsif (nameIn == 'interact reset') then
      interact = Code_evaluation_sandbox.new
      msgOut = {}
    else
      msgOut = { 'error_message' => ['invalid request'] }

    end # if !input

    writeRawData(conn, makeMsg(seqNumIn, nameIn, msgOut))
    STDERR.puts "sent reply #{seqNumIn}, #{nameIn}" #", #{msgOut}"
  end # while

end # def listener_main_loop


############################################################################
# main program
############################################################################


begin
  $lg.debug "calling server accept"
  benchtime = Benchmark.measure {
    @accepted_connection = @server.accept
  }.real
  $lg.debug "got connection after time: #{benchtime}"
rescue Errno::EAGAIN, Errno::ECONNABORTED, Errno::EPROTO, Errno::EINTR
  STDERR.puts "accept error"
  sleep 1
  IO.select([@server])
  retry
end

@server.close
$lg.debug "server closed"

# send hello message with sequence number 0, and information about tdriver and ruby

# following code gets all constants starting with RUBY_ in Object class
@hello_data = Hash[Object.constants.find_all { |c| c.start_with?('RUBY_') }.map { |c| [c, [Object.const_get(c).to_s]]}]

@hello_data['tdriver'] = [ @tdriver_gem_version ]
@hello_data['version'] = [ @tdriver_interface_rb_version ]

begin
  writeRawData(@accepted_connection, makeMsg(0, "hello", @hello_data))
  $lg.debug "hello sent, calling main loop"
  @listener.main_loop(@accepted_connection)
rescue => ex
  STDERR.puts "main program caught an exception: #{ex}\n#{ex.backtrace.join('\n')}"
  $lg.fatal "main program caught an exception: #{ex}\n#{ex.backtrace.join(' <= ')}"
end
$lg.debug "EXIT"

@accepted_connection.close
@accepted_connection = nil

exit 0
