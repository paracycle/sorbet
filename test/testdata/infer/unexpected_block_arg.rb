# typed: true

class Test
  extend T::Sig

  sig {params(x: Integer).void}
  def self.test1(x); end

  def self.test2(x); yield; end
end

Test.test1(10) {|x| nil } # error: Method `Test.test` does not take a block argument

Test.test2(10) {|x| nil }
