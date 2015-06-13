module.exports = function(db, DataTypes) {
  return db.define('Status', {
    id: { type: DataTypes.STRING, primaryKey: true },
    status: { type: DataTypes.ENUM('off', 'on', 'closed', 'open', 'opening', 'closing'), allowNull: false }
  }, { createdAt: false });
}
